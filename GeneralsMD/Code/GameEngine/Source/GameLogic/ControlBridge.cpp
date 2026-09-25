// SPDX-License-Identifier: GPL-3.0-only
#include "PreRTS.h"
#include "GameLogic/ControlBridge.h"
#include "GameLogic/TerrainLogic.h"     // TheTerrainLogic, Waypoint
#include "GameLogic/PolygonTrigger.h"
#include "GameLogic/SidesList.h"        // BuildListInfo (build-list nodes)
#include "GameLogic/GameLogic.h"        // TheGameLogic
#include "GameLogic/Object.h"           // Object, getShroudedStatus/getTemplate/isKindOf
#include "GameLogic/Module/AIUpdate.h"  // AIUpdateInterface::aiAttackMoveToPosition (attack order)
#include "GameLogic/Module/ProductionUpdate.h" // ProductionUpdateInterface, ProductionEntry (Task 7)
#include "GameLogic/Weapon.h"           // NO_MAX_SHOTS_LIMIT
#include "GameLogic/PartitionManager.h" // ThePartitionManager, getShroudStatusForPlayer, CellShroudStatus
#include "GameLogic/VictoryConditions.h" // TheVictoryConditions, hasAchievedVictory/hasBeenDefeated (Task 6)
#include "Common/GameCommon.h"          // CMD_FROM_AI, CELLSHROUD_CLEAR
#include "Common/GlobalData.h"          // TheGlobalData->m_wasmBridgeSide (Task 4.3)
#include "Common/Player.h"              // Player, Money, Energy
#include "Common/PlayerList.h"          // ThePlayerList
#include "Common/ScoreKeeper.h"         // Player::getScoreKeeper() (Task 4)
#include "Common/Team.h"                // ObjectIterateFunc, Team, TeamFactory
#include "Common/ThingTemplate.h"       // ThingTemplate::getName
#include "Common/ThingFactory.h"        // TheThingFactory->findTemplate (train_now precondition mirror)
#include "Common/BuildAssistant.h"      // TheBuildAssistant->isPossibleToMakeUnit (train_now precondition mirror)
#include "Common/KindOf.h"              // KINDOF_*
#include <algorithm>                    // std::sort (threats_near_me ranking, Task 5)
#include <utility>
#include <vector>
#include <list>                         // Player::getPlayerTeams() -> std::list<TeamPrototype*>
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
  const std::vector<Coord3D>* myStructures;   // our structure positions (Task 5)
  std::vector<CountEntry> threats;            // visible enemy units within 500 units of ours
};

// Our own structures' positions, gathered once per observe() so the enemy loop below can
// test each visible enemy unit's proximity to our base (Task 5's threats_near_me).
struct StructPosAccum { std::vector<Coord3D> pos; };
void structPosCb(Object* obj, void* ud)
{
  if (obj && obj->isKindOf(KINDOF_STRUCTURE)) ((StructPosAccum*)ud)->pos.push_back(*obj->getPosition());
}

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

  // threats_near_me: visible, non-structure enemy units within 500 world units (~50 map
  // cells -- a base's footprint) of ANY of our structures.
  if (a->myStructures && !obj->isKindOf(KINDOF_STRUCTURE)) {
    const Coord3D* p = obj->getPosition();
    for (const Coord3D& s : *a->myStructures) {
      const Real dx = p->x - s.x, dy = p->y - s.y;
      if (dx * dx + dy * dy <= 500.0f * 500.0f) { bumpCount(a->threats, nm); break; }
    }
  }
}

// Gather a player's weapon-bearing combat units for `attack` (spec A.4). The
// attack-role filter mirrors observe()'s unit tally but is stricter: a unit
// counts only if it is INFANTRY/VEHICLE/AIRCRAFT, is NOT a dozer/harvester/
// structure, actually bears a weapon (hasAnyWeapon), and has an AI interface we
// can issue the order through. Everything else (builders, resource collectors,
// bases) is excluded so "no attack-role teams available" is a true resource-only
// signal.
struct CombatAccum {
  std::vector<Object*> units;
};

void combatUnitCb(Object* obj, void* ud)
{
  CombatAccum* a = (CombatAccum*)ud;
  if (!obj) return;
  if (obj->isKindOf(KINDOF_STRUCTURE)) return;
  if (obj->isKindOf(KINDOF_DOZER))     return;
  if (obj->isKindOf(KINDOF_HARVESTER)) return;
  if (!(obj->isKindOf(KINDOF_INFANTRY) || obj->isKindOf(KINDOF_VEHICLE)
     || obj->isKindOf(KINDOF_AIRCRAFT))) return;
  if (!obj->hasAnyWeapon())            return;   // weapon-bearing only
  if (!obj->getAIUpdateInterface())    return;   // must be commandable
  a->units.push_back(obj);
}

// Find ONE cheap/mobile unit for `scout` (spec A.5): the first INFANTRY or
// VEHICLE object that is NOT a dozer/harvester/structure and has an AI
// interface we can issue a MOVE order through. A dedicated recon unit would
// be ideal (fastest/cheapest scout template), but any one mobile unit is fine
// for v1 -- picking the FIRST such unit found keeps the op O(1) instead of
// requiring a cost/speed comparison table we don't have yet.
struct ScoutAccum {
  Object* unit;   // first match, or NULL
};

void scoutUnitCb(Object* obj, void* ud)
{
  ScoutAccum* a = (ScoutAccum*)ud;
  if (a->unit) return;   // already found one -- first match wins
  if (!obj) return;
  if (obj->isKindOf(KINDOF_STRUCTURE)) return;
  if (obj->isKindOf(KINDOF_DOZER))     return;
  if (obj->isKindOf(KINDOF_HARVESTER)) return;
  if (!(obj->isKindOf(KINDOF_INFANTRY) || obj->isKindOf(KINDOF_VEHICLE))) return;
  if (!obj->getAIUpdateInterface())    return;   // must be commandable
  a->unit = obj;
}

// Production queue scrape (Task 7): one entry per in-progress unit/upgrade across all of our
// own objects' ProductionUpdateInterface queues.
struct ProdAccum { AsciiString json; Int n; };
void prodCb(Object* obj, void* ud)
{
  ProdAccum* a = (ProdAccum*)ud;
  ProductionUpdateInterface* pu = obj ? obj->getProductionUpdateInterface() : NULL;
  if (!pu) return;
  for (const ProductionEntry* e = pu->firstProduction(); e; e = pu->nextProduction(e)) {
    const Bool isUnit = e->getProductionObject() != NULL;
    const AsciiString item = isUnit ? e->getProductionObject()->getName()
                                    : (e->getProductionUpgrade() ? e->getProductionUpgrade()->getUpgradeName() : AsciiString("unknown"));
    if (a->n++) a->json.concat(',');
    a->json.concat("{\"factory\":\""); jsonEscape(a->json, obj->getTemplate()->getName().str());
    a->json.concat("\",\"item\":\""); jsonEscape(a->json, item.str());
    AsciiString t; t.format("\",\"kind\":\"%s\",\"percent\":%d,\"remaining\":%d}",
                            isUnit ? "unit" : "upgrade", (Int)e->getPercentComplete(), e->getProductionQuantityRemaining());
    a->json.concat(t);
  }
}

} // anonymous namespace

// ---------------------------------------------------------------------------

void ControlBridge::init(Bool active)
{
  // A network match is deterministic lockstep: every peer must apply the same commands in the
  // same frame. The bridge writes one peer's state directly (build list, team priorities, unit
  // orders), so in a LAN/Internet game it would desync the match on the first op. Stay inert.
  m_active = active;
  if (!m_active) {
    fprintf(stderr, "[BRIDGE] inactive: multiplayer match (the bridge steers single-machine skirmishes only)\n");
    fflush(stderr);
  }
  // init() runs for every map load (shell map, then each match), and the bridge object
  // outlives them. Forget the previous game's binding, or tick() never rebinds and every
  // op keeps acting on a player index from the last match.
  m_bridgePlayerIndex = -1;
  m_haveLastLosses = false;
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
  fprintf(stderr, "[BRIDGE] %zu regions derived\n", m_regions.size());
  fflush(stderr);

  // Task 8: semantic region aliases. Collect the Player_N_Start waypoints (public
  // knowledge in an RTS -- shown on the map-select screen) so observe() can expose
  // "my_base" and "enemy_start_N" ahead of the ~250 raw waypoint/trigger names.
  // m_myStart is resolved lazily (resolveMyStart) once the bridge player's command
  // center exists, which is after the bind -- never here.
  m_starts.clear(); m_myStart = -1;
  for (Int n = 1; n <= MAX_PLAYER_COUNT; ++n) {
    AsciiString wn; wn.format("Player_%d_Start", n);
    for (const BridgeRegion& r : m_regions) if (r.name == wn) { m_starts.push_back(r); break; }
  }
}

// ---------------------------------------------------------------------------
// resolveMyStart (Task 8): identify which Player_N_Start waypoint is "my_base" --
// the one nearest our command center (KINDOF_COMMANDCENTER). Resolved lazily
// because the command center doesn't exist at init() time (map load); called
// from observe() every frame until it succeeds (m_myStart >= 0), then a no-op.
// A captureless lambda converts to ObjectIterateFunc's C function-pointer type.
// ---------------------------------------------------------------------------
void ControlBridge::resolveMyStart(Player* me)
{
  if (m_myStart >= 0 || m_starts.empty() || !me) return;
  struct CC { Coord3D p; Bool found; } cc = { {0,0,0}, FALSE };
  me->iterateObjects([](Object* o, void* ud) {
    CC* c = (CC*)ud;
    if (!c->found && o && o->isKindOf(KINDOF_COMMANDCENTER)) { c->p = *o->getPosition(); c->found = TRUE; }
  }, &cc);
  if (!cc.found) return;   // try again next observe
  Real best = 1e30f;
  for (size_t i = 0; i < m_starts.size(); ++i) {
    const Real dx = m_starts[i].center.x - cc.p.x, dy = m_starts[i].center.y - cc.p.y, d = dx*dx + dy*dy;
    if (d < best) { best = d; m_myStart = (Int)i; }
  }
}

// ---------------------------------------------------------------------------
// Minimal envelope JSON reader. The wire request is the fixed shape
//   {"id":<int>,"op":"<str>","args":{...}}
// (settled in Task 2.1; the Node side emits id/op BEFORE args — see
// control-channel.mjs send()). We avoid a JSON library. The top-level scalars
// `id` and `op` are read by scanning for a top-level "key": and taking the
// scalar after it: safe because id/op precede args, so a first-match scan can't
// be shadowed by a same-named key nested inside args. `args` is NOT flat, so it
// is read depth/scope-aware instead — apply() locates the "args" object,
// brace-matches its extent, and reads keys only from WITHIN those bounds (see
// jsonMatchBracket / jsonReadStringArrayIn). This keeps a nested key from being
// confused with a top-level one and vice-versa.
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

// ---------------------------------------------------------------------------
// Depth/scope-aware helpers for reading structured `args`. These operate on
// explicit [begin,end) bounds (or brace-matched extents) so a nested key can't
// be confused with a top-level one — unlike the flat scalar readers above.
// ---------------------------------------------------------------------------

// Given `p` pointing at an opening '{' or '[', return the matching close char,
// respecting nested braces/brackets of the SAME kind and skipping string bodies
// (so a brace inside a "string" never counts). NULL if unbalanced.
static const char* jsonMatchBracket(const char* p)
{
  if (!p || (*p != '{' && *p != '[')) return NULL;
  const char open = *p;
  const char close = (open == '{') ? '}' : ']';
  Int depth = 0;
  Bool inStr = FALSE;
  for (; *p; ++p) {
    const char c = *p;
    if (inStr) {
      if (c == '\\' && p[1]) { ++p; continue; }   // skip escaped char
      if (c == '"') inStr = FALSE;
      continue;
    }
    if (c == '"') { inStr = TRUE; continue; }
    if (c == open) ++depth;
    else if (c == close) { if (--depth == 0) return p; }
  }
  return NULL;
}

// Read a string array `"<key>":[ "a", "b", ... ]` located strictly WITHIN
// [begin,end). Returns FALSE if the key/array isn't found in range.
static Bool jsonReadStringArrayIn(const char* begin, const char* end,
                                  const char* key, std::vector<AsciiString>& out)
{
  AsciiString pat; pat.format("\"%s\"", key);
  const Int patLen = pat.getLength();
  const char* p = NULL;
  for (const char* q = begin; q + patLen <= end; ++q) {
    if (strncmp(q, pat.str(), patLen) == 0) { p = q; break; }
  }
  if (!p) return FALSE;
  p += patLen;
  while (p < end && *p != ':') ++p;
  if (p >= end || *p != ':') return FALSE;
  ++p;
  while (p < end && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) ++p;
  if (p >= end || *p != '[') return FALSE;
  const char* arrEnd = jsonMatchBracket(p);
  if (!arrEnd || arrEnd > end) return FALSE;
  ++p;                                            // step past '['
  while (p < arrEnd) {
    while (p < arrEnd && *p != '"') ++p;          // advance to next string start
    if (p >= arrEnd || *p != '"') break;
    ++p;                                          // step past opening quote
    AsciiString s;
    for (; p < arrEnd && *p != '"'; ++p) {
      if (*p == '\\' && p[1]) {
        ++p;
        switch (*p) {
          case 'n': s.concat('\n'); break;
          case 't': s.concat('\t'); break;
          case 'r': s.concat('\r'); break;
          default:  s.concat(*p);   break;
        }
      } else {
        s.concat(*p);
      }
    }
    if (p < arrEnd && *p == '"') ++p;             // step past closing quote
    out.push_back(s);
  }
  return TRUE;
}

// ---------------------------------------------------------------------------
// Bounded scalar readers, mirroring jsonFindValue/jsonReadInt/jsonReadStr but
// confined to [begin,end) instead of scanning a NUL-terminated string. Needed
// to read "unit"/"priority"/"count" out of ONE array-element object without
// spilling into a sibling object that happens to share a key name (set_build_list
// only ever needed one flat array; set_team_priorities needs an array of
// objects, so each object is its own bounded scope).
// ---------------------------------------------------------------------------
static const char* jsonFindValueIn(const char* begin, const char* end, const char* key)
{
  AsciiString pat; pat.format("\"%s\"", key);
  const Int patLen = pat.getLength();
  for (const char* q = begin; q + patLen <= end; ++q) {
    if (strncmp(q, pat.str(), patLen) == 0) {
      const char* p = q + patLen;
      while (p < end && *p != ':') ++p;
      if (p >= end || *p != ':') return NULL;
      ++p;
      while (p < end && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) ++p;
      return p;
    }
  }
  return NULL;
}

static Bool jsonReadIntIn(const char* begin, const char* end, const char* key, Int& out)
{
  const char* p = jsonFindValueIn(begin, end, key);
  if (!p || p >= end) return FALSE;
  out = atoi(p);
  return TRUE;
}

static Bool jsonReadStrIn(const char* begin, const char* end, const char* key, AsciiString& out)
{
  const char* p = jsonFindValueIn(begin, end, key);
  if (!p || p >= end || *p != '"') return FALSE;
  ++p;
  out.clear();
  for (; p < end && *p != '"'; ++p) {
    if (*p == '\\' && p + 1 < end) {
      ++p;
      switch (*p) {
        case 'n': out.concat('\n'); break;
        case 't': out.concat('\t'); break;
        case 'r': out.concat('\r'); break;
        default:  out.concat(*p);   break;
      }
    } else {
      out.concat(*p);
    }
  }
  return TRUE;
}

// One requested team-priority entry: {"unit":"<name>","priority":<int>,"count":<int?>}.
struct TeamPriorityReq { AsciiString unit; Int priority; Int count; };

// Read `"teams":[ {...}, {...} ]` located strictly WITHIN [begin,end). Walks the
// array brace-matching each element object (jsonMatchBracket, same helper the
// array-of-strings reader uses for the array itself) and reads unit/priority/
// count from within THAT object's bounds only. Entries missing unit or
// priority are skipped (not pushed) rather than pushed with garbage defaults.
static Bool jsonReadTeamsArrayIn(const char* begin, const char* end,
                                 std::vector<TeamPriorityReq>& out)
{
  const char* p = jsonFindValueIn(begin, end, "teams");
  if (!p || p >= end || *p != '[') return FALSE;
  const char* arrEnd = jsonMatchBracket(p);
  if (!arrEnd || arrEnd > end) return FALSE;
  ++p;                                             // step past '['
  while (p < arrEnd) {
    while (p < arrEnd && *p != '{') ++p;
    if (p >= arrEnd) break;
    const char* objEnd = jsonMatchBracket(p);
    if (!objEnd || objEnd > arrEnd) break;
    TeamPriorityReq req; req.priority = 0; req.count = 0;
    if (jsonReadStrIn(p, objEnd, "unit", req.unit) &&
        jsonReadIntIn(p, objEnd, "priority", req.priority)) {
      jsonReadIntIn(p, objEnd, "count", req.count);  // optional, unused in v1 (see apply())
      out.push_back(req);
    }
    p = objEnd + 1;
  }
  return TRUE;
}

// ---------------------------------------------------------------------------
// Logical structure name <-> ThingTemplate keyword table.
//
// The MCP tool speaks faction-agnostic logical names (spec A.2 enum). Build-list
// nodes store faction-specific ThingTemplate names (e.g. "ChinaWarFactory").
// v1 maps a logical name to whichever build-list node's template CONTAINS one of
// its keywords (case-insensitive). This is deliberately heuristic: a full
// per-faction template table is deferred to a later task. Callers may also pass
// a raw template name (or any substring of one), which resolves directly.
// ---------------------------------------------------------------------------
struct LogicalMap { const char* logical; const char* keywords; };  // space-separated keywords
// Keywords cover all three factions' shipped template names (verified against the packed
// assets: AmericaPowerPlant/ChinaPowerPlant, GLASupplyStash, GLAArmsDealer, ChinaGattlingCannon
// (two t's), GLAStingerSite, GLATunnelNetwork, ChinaNuclearMissileLauncher, GLAScudStorm ...).
static const LogicalMap kLogicalMap[] = {
  { "power_plant",    "PowerPlant ColdFusion Reactor" },
  { "supply_center",  "SupplyCenter SupplyStash SupplyDepot" },
  { "barracks",       "Barracks" },
  { "war_factory",    "WarFactory ArmsDealer" },
  { "airfield",       "Airfield Helipad" },
  { "defense_turret", "Gatling Gattling Stinger Firebase Bunker Patriot Tunnel" },
  { "superweapon",    "ParticleCannon MissileSilo ScudStorm NuclearMissile Nuke" },
};

// Finding D: alias parsing must accept "enemy_start_<digits>" only, not
// "enemy_start_1abc" (atoi() alone silently accepts and truncates trailing junk).
// `s` must be non-empty and every character a decimal digit.
static Bool isAllDigits(const char* s)
{
  if (!s || !*s) return FALSE;
  for (; *s; ++s) if (*s < '0' || *s > '9') return FALSE;
  return TRUE;
}

// Case-insensitive: does `hay` contain `needle` as a substring?
static Bool ciContains(const char* hay, const char* needle)
{
  if (!hay || !needle || !*needle) return FALSE;
  for (const char* h = hay; *h; ++h) {
    Int i = 0;
    for (; needle[i] && h[i]; ++i) {
      char a = h[i], b = needle[i];
      if (a >= 'A' && a <= 'Z') a += 32;
      if (b >= 'A' && b <= 'Z') b += 32;
      if (a != b) break;
    }
    if (!needle[i]) return TRUE;
  }
  return FALSE;
}

// Case-insensitive: does `tmpl` contain any space-separated token of `keywords`?
static Bool matchesAnyKeyword(const char* tmpl, const char* keywords)
{
  char tok[64];
  for (const char* k = keywords; *k; ) {
    while (*k == ' ') ++k;
    Int t = 0;
    while (*k && *k != ' ' && t < (Int)sizeof(tok) - 1) tok[t++] = *k++;
    tok[t] = 0;
    if (t > 0 && ciContains(tmpl, tok)) return TRUE;
  }
  return FALSE;
}

// Does the build-list node template `tmpl` satisfy the requested name `req`
// (a logical enum name, or a raw template name / substring)?
static Bool templateMatchesRequest(const char* tmpl, const AsciiString& req)
{
  for (const LogicalMap& m : kLogicalMap)
    if (req == m.logical) return matchesAnyKeyword(tmpl, m.keywords);
  return ciContains(tmpl, req.str());             // unknown logical -> raw template match
}

// Reverse map: the logical name a template resolves to, or NULL if none.
static const char* logicalForTemplate(const char* tmpl)
{
  for (const LogicalMap& m : kLogicalMap)
    if (matchesAnyKeyword(tmpl, m.keywords)) return m.logical;
  return NULL;
}

// ---------------------------------------------------------------------------
// Logical unit name <-> team-member ThingTemplate keyword table (spec A.3's
// faction-agnostic `unit` enum: ranger/missile_defender/crusader_tank/humvee/
// tomahawk/raptor_jet). set_team_priorities matches these against each
// TeamTemplateInfo::m_unitsInfo[].unitThingName (the unit types a team is
// composed of), NOT against build-list structures. The names are USA-flavoured
// but each row lists the ROLE's template for all three factions, because the
// bridge player is normally the China AI (and a GLA AI is one -aidifficulty
// setup away): a USA-only table made every schema-valid set_team_priorities
// call fail with "no requested units matched any team". Keywords verified
// against the packed assets (`strings assets.data`): AmericaInfantryRanger,
// ChinaInfantryRedguard, GLAInfantryRebel, AmericaInfantryMissileDefender,
// ChinaInfantryTankHunter, GLAInfantryTunnelDefender, AmericaTankCrusader,
// ChinaTankBattleMaster, GLATankScorpion, AmericaVehicleHumvee,
// ChinaTankGattling, GLAVehicleTechnical, AmericaVehicleTomahawk,
// ChinaVehicleInfernoCannon, ChinaVehicleNukeLauncher, GLAVehicleScudLauncher,
// AmericaJetRaptor, ChinaJetMIG (GLA has no jet: raptor_jet is dropped for GLA).
// Raw/substring template names still resolve directly via the ciContains fallback.
// ---------------------------------------------------------------------------
static const LogicalMap kUnitLogicalMap[] = {
  { "ranger",           "InfantryRanger Redguard InfantryRebel" },
  { "missile_defender", "MissileDefender TankHunter TunnelDefender" },
  { "crusader_tank",    "Crusader BattleMaster Scorpion" },
  { "humvee",           "Humvee TankGattling VehicleTechnical" },
  { "tomahawk",         "Tomahawk InfernoCannon NukeLauncher ScudLauncher" },
  { "raptor_jet",       "JetRaptor JetMIG" },
};

// Does a team's unit-slot template `tmpl` satisfy the requested `req` (a
// logical unit enum name, or a raw template name / substring)?
static Bool unitTemplateMatchesRequest(const char* tmpl, const AsciiString& req)
{
  for (const LogicalMap& m : kUnitLogicalMap)
    if (req == m.logical) return matchesAnyKeyword(tmpl, m.keywords);
  return ciContains(tmpl, req.str());
}

// train_now precondition mirror (Important A / finding A): does the bridge player own a
// buildable-from factory for `thing`? This mirrors AIPlayer::findFactory(thing, /*busyOK=*/true)
// (AIPlayer.cpp:1428-1462) closely enough to reproduce its ONLY failure signal (no build-list
// object can make this thing at all) without touching AIPlayer's protected members: walk the
// player's build-list objects, skip ones under construction/being sold or not ours, and accept
// the first whose ProductionUpdateInterface + TheBuildAssistant->isPossibleToMakeUnit() agrees it
// could make `thing` -- busy or not (busyOK=true), since a busy-but-valid factory still means
// "queueable", matching what buildSpecificAITeam actually gates on.
static Bool hasFactoryForThing(Player* p, const ThingTemplate* thing)
{
  if (!p || !thing || !TheGameLogic || !TheBuildAssistant) return FALSE;
  for (BuildListInfo* info = p->getBuildList(); info; info = info->getNext()) {
    Object* factory = TheGameLogic->findObjectByID(info->getObjectID());
    if (!factory) continue;
    if (factory->getControllingPlayer() != p) continue;
    if (factory->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION)) continue;
    if (factory->testStatus(OBJECT_STATUS_SOLD)) continue;
    if (!factory->getProductionUpdateInterface()) continue;
    if (TheBuildAssistant->isPossibleToMakeUnit(factory, thing)) return TRUE;
  }
  return FALSE;
}

// Does `pp` own at least one team prototype with a real unit composition
// (m_numUnitsInfo > 0)? EVERY player -- human or AI -- always owns an implicit
// singleton "team<playername>" wrapping their own directly-owned objects, so
// getPlayerTeams()->empty() is never a useful signal here (unlike build lists,
// which really are NULL for humans). set_team_priorities needs a player that
// owns actual PRODUCTION team templates (composed of real unit slots) to have
// anything meaningful to prioritize.
static Bool hasProductionTeams(Player* pp)
{
  if (!pp || !pp->getPlayerTeams()) return FALSE;
  for (TeamPrototype* proto : *pp->getPlayerTeams())
    if (proto && proto->getTemplateInfo()->m_numUnitsInfo > 0) return TRUE;
  return FALSE;
}

void ControlBridge::tick()
{
  if (!m_active || !TheGameLogic || !ThePlayerList) return;

  // Bind the bridge player once, at match start (Phase 4.2). The design steers
  // an AISkirmishPlayer's knobs, so the bridge player MUST be the "knob-having"
  // skirmish AI: the one that owns BOTH a build list (set_build_list steers it)
  // AND production team prototypes (set_team_priorities steers them) and gets an
  // army built (attack/scout command it). The human slot has none of these
  // (getBuildList() is NULL for PLAYER_HUMAN, and it owns only the implicit
  // singleton team), so binding to the human — as the pre-4.2 default did — left
  // the production ops with nothing to steer and, worse, made observe() read the
  // human's fog while the write-ops fell back to the AI's units (concern C4:
  // scouting the AI's unit reveals nothing in the human's observe()). Binding
  // observe() and all four write-ops to ONE knob-having AI fixes that.
  //
  // Done lazily here (not in the GameEngine bootstrap) because build lists and
  // team prototypes only exist AFTER the players are populated, which is later
  // than the bootstrap. We bind to the FIRST player that owns a build list AND
  // production teams (deterministic). If a build-list owner exists but its teams
  // aren't set up yet, we WAIT (leave the index unset and retry next frame)
  // rather than mis-bind. Only when the map has no build-list owner at all (a
  // non-skirmish scenario) do we fall back to the first base owner.
  if (m_bridgePlayerIndex < 0) {
    // Task 4.3: the bridge match now has TWO skirmish AIs (the bridge-steered AI
    // AND a fighting opponent), so both own build lists + production teams and
    // "first build-list owner" is ambiguous. The bootstrap records the bridge
    // faction's side in m_wasmBridgeSide; when set, bind ONLY to the build-list
    // owner whose side matches it (the bridge AI), never the opponent. When
    // empty (non-bridge maps / legacy) fall back to the first build-list owner.
    const AsciiString wantSide =
        TheGlobalData ? TheGlobalData->m_wasmBridgeSide : AsciiString();
    Bool sawBuildListOwner = FALSE;
    for (Int pi = 0; pi < ThePlayerList->getPlayerCount(); ++pi) {
      Player* p = ThePlayerList->getNthPlayer(pi);
      if (!p || !p->getBuildList()) continue;         // human/base-less players excluded
      if (wantSide.isNotEmpty() && p->getSide().compareNoCase(wantSide) != 0)
        continue;                                     // skip the opponent AI: wrong faction
      sawBuildListOwner = TRUE;
      if (hasProductionTeams(p)) {
        m_bridgePlayerIndex = p->getPlayerIndex();
        fprintf(stderr, "[BRIDGE] bound bridge player = %d (side '%s', skirmishAI=%d)\n",
                m_bridgePlayerIndex, p->getSide().str(), p->isSkirmishAIPlayer() ? 1 : 0);
        fflush(stderr);
        break;
      }
    }
    // Base-owner fallback only when NO bridge side was designated. With a
    // designated side we WAIT for that AI rather than risk binding the opponent.
    if (m_bridgePlayerIndex < 0 && !sawBuildListOwner && wantSide.isEmpty()) {
      for (Int pi = 0; pi < ThePlayerList->getPlayerCount(); ++pi) {
        Player* p = ThePlayerList->getNthPlayer(pi);
        if (p && p->countBuildings() > 0) { m_bridgePlayerIndex = p->getPlayerIndex(); break; }
      }
    }
  }

#ifdef __EMSCRIPTEN__
  // Drain at most one queued control request per frame. gxControl.poll() copies
  // the next request into our stack buffer and returns its byte length (0 =
  // none). v1 ops are tiny JSON; 8192 comfortably exceeds any request. We offer
  // one byte less than the buffer so the terminator always fits; poll() answers
  // an oversized request with an error reply itself and returns 0.
  char buf[8192];
  Int n = MAIN_THREAD_EM_ASM_INT(
    { return (typeof gxControl !== 'undefined') ? gxControl.poll($0, $1) : 0; },
    (Int)buf, (Int)sizeof(buf) - 1);
  if (n <= 0 || n >= (Int)sizeof(buf)) return;
  buf[n] = 0;

  Int id = 0;
  AsciiString op;
  jsonReadInt(buf, "id", id);
  jsonReadStr(buf, "op", op);

  // Dispatch. `observe` is the read-op; `set_build_list`, `set_team_priorities`,
  // `attack` and `scout` are Phase 3 write-ops routed through apply(); every
  // other op still gets an explicit "not implemented" reply (coverage, never a
  // silent drop).
  // The request is already off the browser queue, so a throw from an op must still
  // produce a reply, or the caller waits out its timeout for nothing.
  AsciiString result;
  try {
    if (op == "observe") {
      if (m_bridgePlayerIndex < 0) {
        // Before the bind the only player we could read is index 0, the neutral player. Say so
        // rather than hand the model someone else's cash, base and fog.
        AsciiString nr;
        nr.format("{\"ready\":false,\"reason\":\"bridge player not bound yet\",\"frame\":%u}",
                  (unsigned)TheGameLogic->getFrame());
        result = nr;
      } else {
        result = observe(m_bridgePlayerIndex);
        if (result.isEmpty()) {
          AsciiString nr;
          nr.format("{\"ready\":false,\"reason\":\"bound player not found\",\"frame\":%u}",
                    (unsigned)TheGameLogic->getFrame());
          result = nr;
        }
      }
    } else if (op == "set_build_list" || op == "set_team_priorities" || op == "attack" || op == "scout" ||
               op == "build_now" || op == "train_now") {
      result = apply(op, buf);
    } else {
      result = "{\"accepted\":false,\"reason\":\"op not implemented (Phase 3)\"}";
    }
  } catch (...) {
    fprintf(stderr, "[BRIDGE] exception in op '%s' (id=%d)\n", op.str(), id);
    result = "{\"accepted\":false,\"reason\":\"exception in bridge op\"}";
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

  // Combat state (live match 2026-09-24: the base was dismantled while this said false).
  const UnsignedInt now = TheGameLogic->getFrame();
  const UnsignedInt atk = me->getAttackedFrame();
  const Bool everAttacked = atk != 0;
  const Real agoSec = everAttacked ? (Real)(now - atk) / LOGICFRAMES_PER_SECOND : -1.0f;
  const Bool underAttack = everAttacked && agoSec <= 10.0f;
  // attackers_this_match: cumulative for the whole match (Player::m_attackedBy[] has no
  // timestamps), enemies only — same relationship test the enemy section below uses, so a
  // teammate or self hit (ActiveBody.cpp:607 sets the flag with no relationship check) never
  // appears here. Recency is what under_attack/last_attacked_seconds_ago are for.
  AsciiString attackers = "[";
  for (Int pi = 0, n = 0; pi < ThePlayerList->getPlayerCount(); ++pi) {
    if (pi == playerIndex || !me->getAttackedBy(pi)) continue;
    Player* attacker = ThePlayerList->getNthPlayer(pi);
    if (!attacker || me->getRelationship(attacker->getDefaultTeam()) != ENEMIES) continue;
    AsciiString one; one.format("%s%d", n++ ? "," : "", pi); attackers.concat(one);
  }
  attackers.concat("]");

  // Match result (Task 6): derived from TheVictoryConditions, not a constant.
  const char* match = "ongoing";
  if (TheVictoryConditions) {
    if (TheVictoryConditions->hasAchievedVictory(me)) match = "won";
    else if (TheVictoryConditions->hasBeenDefeated(me)) match = "lost";
  }

  // speed (finding D): real logic-time scale, not a hard-coded 1.00. Slow-mo (-slowmo/-bridge)
  // only takes effect when m_wasmSlowmoSkirmish is set AND the requested fps is below real time
  // (GameEngine.cpp:1014-1019); mirror that gate here rather than reporting m_wasmSlowmoFps/30
  // unconditionally (e.g. -slowmofps 30 or slow-mo simply off must both read as 1.00).
  Real speed = 1.0f;
  if (TheGlobalData && TheGlobalData->m_wasmSlowmoSkirmish &&
      TheGlobalData->m_wasmSlowmoFps > 0 && TheGlobalData->m_wasmSlowmoFps < LOGICFRAMES_PER_SECOND) {
    speed = (Real)TheGlobalData->m_wasmSlowmoFps / (Real)LOGICFRAMES_PER_SECOND;
  }

  // Top-level + self.
  tmp.format("{\"ready\":true,\"frame\":%u,\"speed\":%.2f,\"match\":\"%s\",\"self\":{\"faction\":\"",
             (unsigned)TheGameLogic->getFrame(), speed, match);
  j.concat(tmp);
  jsonEscape(j, me->getSide().str());
  tmp.format("\",\"cash\":%u,\"power\":{\"produced\":%d,\"consumed\":%d,\"surplus\":%d},"
             "\"under_attack\":%s,\"last_attacked_seconds_ago\":",
             (unsigned)cash, produced, consumed, surplus, underAttack ? "true" : "false");
  j.concat(tmp);
  if (everAttacked) { tmp.format("%.1f", agoSec); j.concat(tmp); } else j.concat("null");
  j.concat(",\"attackers_this_match\":"); j.concat(attackers); j.concat("}");

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

  // production (Task 7): scrape each own object's ProductionUpdateInterface queue.
  ProdAccum pa; pa.n = 0; me->iterateObjects(prodCb, &pa);
  j.concat(",\"production\":["); j.concat(pa.json); j.concat("]");

  // build_plan: the AI's own build list, in order. `queued` is the priority flag build_now
  // sets, so it is the direct read-back of a build_now order (Follow-up Task 2).
  j.concat(",\"build_plan\":[");
  Bool firstB = TRUE;
  for (BuildListInfo* n = me->getBuildList(); n; n = n->getNext()) {
    if (!firstB) j.concat(',');
    firstB = FALSE;
    const Bool built = TheGameLogic->findObjectByID(n->getObjectID()) != NULL;
    j.concat("{\"template\":\""); jsonEscape(j, n->getTemplateName().str());
    tmp.format("\",\"built\":%s,\"queued\":%s}", built ? "true" : "false", n->isPriorityBuild() ? "true" : "false");
    j.concat(tmp);
  }
  j.concat("]");

  // enemy — fog-limited. One entry per enemy player; tallies count ONLY objects
  // currently visible to the observer (see enemyObjectCb's shroud gate).
  j.concat(",\"enemy\":{\"players\":[");
  Bool firstEnemy = TRUE;
  StructPosAccum mine; me->iterateObjects(structPosCb, &mine);
  for (Int pi = 0; pi < ThePlayerList->getPlayerCount(); ++pi) {
    Player* other = ThePlayerList->getNthPlayer(pi);
    if (!other || other == me) continue;
    if (me->getRelationship(other->getDefaultTeam()) != ENEMIES) continue;

    EnemyAccum ea; ea.observerIndex = playerIndex; ea.visibleObjects = 0;
    ea.myStructures = &mine.pos;
    other->iterateObjects(enemyObjectCb, &ea);

    if (!firstEnemy) j.concat(',');
    firstEnemy = FALSE;
    // is_ai: TRUE when this enemy is a fighting AISkirmishPlayer (Task 4.3). Lets
    // an observer confirm the match has a real adversary, not an idle human slot.
    tmp.format("{\"player\":%d,\"is_ai\":%s,\"faction\":\"",
               pi, other->isSkirmishAIPlayer() ? "true" : "false");
    j.concat(tmp);
    jsonEscape(j, other->getSide().str());
    j.concat("\",\"last_seen\":{\"structures_seen\":");
    writeCountMap(j, ea.structures);
    j.concat(",\"army_seen\":");
    writeCountMap(j, ea.army);
    std::sort(ea.threats.begin(), ea.threats.end(),
              [](const CountEntry& a, const CountEntry& b){ return a.count > b.count; });
    tmp.format("},\"visible_objects\":%d,\"threats_near_me\":[", ea.visibleObjects);
    j.concat(tmp);
    for (size_t t = 0; t < ea.threats.size(); ++t) {
      if (t) j.concat(',');
      j.concat("{\"type\":\""); jsonEscape(j, ea.threats[t].name.str());
      AsciiString c; c.format("\",\"count\":%d}", ea.threats[t].count); j.concat(c);
    }
    j.concat("]}");
  }
  j.concat("]}");

  // losses — cumulative + deltas since the last observe (Task 4). Baseline rule: the first
  // observe after a bind (m_haveLastLosses false) reports deltas equal to the cumulative
  // values, so summing every reply's deltas always reproduces the cumulative counters.
  ScoreKeeper* sk = me->getScoreKeeper();
  LossCounters cur = { sk->getTotalUnitsLost(), sk->getTotalBuildingsLost(), sk->getTotalUnitsDestroyed(),
                       sk->getTotalBuildingsDestroyed(), sk->getTotalUnitsBuilt(), sk->getTotalBuildingsBuilt() };
  const LossCounters base = m_haveLastLosses ? m_lastLosses : LossCounters{0,0,0,0,0,0};
  tmp.format(",\"losses\":{\"units_lost\":%d,\"buildings_lost\":%d,\"units_destroyed\":%d,"
             "\"buildings_destroyed\":%d,\"units_built\":%d,\"buildings_built\":%d}",
             cur.unitsLost, cur.buildingsLost, cur.unitsDestroyed, cur.buildingsDestroyed, cur.unitsBuilt, cur.buildingsBuilt);
  j.concat(tmp);
  tmp.format(",\"losses_since_last_observe\":{\"units_lost\":%d,\"buildings_lost\":%d,\"units_destroyed\":%d,"
             "\"buildings_destroyed\":%d,\"units_built\":%d,\"buildings_built\":%d}",
             cur.unitsLost - base.unitsLost, cur.buildingsLost - base.buildingsLost,
             cur.unitsDestroyed - base.unitsDestroyed, cur.buildingsDestroyed - base.buildingsDestroyed,
             cur.unitsBuilt - base.unitsBuilt, cur.buildingsBuilt - base.buildingsBuilt);
  j.concat(tmp);
  m_lastLosses = cur; m_haveLastLosses = true;

  // map — named regions derived at init() (waypoints + polygon triggers), with
  // Task 8's semantic aliases ("my_base", "enemy_start_N") listed first.
  resolveMyStart(me);
  j.concat(",\"map\":{\"regions\":[");
  Bool firstR = TRUE;
  // Fix round 1 (Important #1): while m_myStart is unresolved (-1) we don't yet
  // know which start is ours, so emitting the OTHER starts as "enemy_start_N"
  // would risk labeling our own base as an enemy's. Emit NO start aliases at
  // all until my_base resolves; raw region names are unaffected.
  if (m_myStart >= 0) {
    j.concat("\"my_base\""); firstR = FALSE;
    for (size_t i = 0, e = 1; i < m_starts.size(); ++i) {
      if ((Int)i == m_myStart) continue;
      AsciiString a; a.format("%s\"enemy_start_%u\"", firstR ? "" : ",", (unsigned)e++); j.concat(a); firstR = FALSE;
    }
  }
  for (const BridgeRegion& r : m_regions) {
    if (!firstR) j.concat(',');
    firstR = FALSE;
    j.concat('"'); jsonEscape(j, r.name.str()); j.concat('"');
  }
  j.concat("]}");

  j.concat("}");
  return j;
}

// ---------------------------------------------------------------------------
// apply(): Phase 3 write-ops. Currently handles `set_build_list` (spec A.2
// set_build_priorities): reorder the bridge player's BuildList to the requested
// structure priority.
//
// Player-selection note (v1): the build list is a PLAYER_COMPUTER structure; a
// human bridge player has none. So we prefer the bridge player's own list, but
// fall back to the first player that owns a non-empty build list (the skirmish
// AI). Phase 4 formalizes the bridge player; this keeps the op meaningful in the
// current human-vs-AI headless boot.
//
// Safety: setBuildList() deletes the previous list (BuildListInfo's destructor
// frees the whole chain — see SidesList.cpp). So we reorder a fresh duplicate()
// of the list and install THAT; the original is then safely deleted. Never
// re-install reused original nodes (Phase 0's spike aliased them, which only
// survived because the pool didn't reclaim immediately).
// ---------------------------------------------------------------------------
AsciiString ControlBridge::apply(const AsciiString& op, const char* req)
{
  if (op == "set_build_list") {
    // 1) Parse args.order[] depth/scope-aware: locate the "args" object, brace-
    //    match its extent, and read "order" only from WITHIN those bounds.
    std::vector<AsciiString> order;
    const char* argsVal = jsonFindValue(req, "args");
    if (argsVal && *argsVal == '{') {
      const char* argsEnd = jsonMatchBracket(argsVal);
      if (argsEnd) jsonReadStringArrayIn(argsVal, argsEnd, "order", order);
    }
    if (order.empty())
      return "{\"accepted\":false,\"reason\":\"missing or empty args.order[]\"}";

    if (!ThePlayerList)
      return "{\"accepted\":false,\"reason\":\"no player list\"}";

    // 2) Pick the target player. AUTHORITATIVE on m_bridgePlayerIndex (Phase 4.2):
    //    when the bridge player is bound, act on THAT player only — never fall
    //    back to "first build-list owner", so the op steers exactly the player
    //    observe() reports (C4). The fallback scan is used only when the bridge
    //    player is unset (m_bridgePlayerIndex < 0).
    Player* p = NULL;
    if (m_bridgePlayerIndex >= 0) {
      p = ThePlayerList->getNthPlayer(m_bridgePlayerIndex);
    } else {
      for (Int pi = 0; pi < ThePlayerList->getPlayerCount(); ++pi) {
        Player* q = ThePlayerList->getNthPlayer(pi);
        if (q && q->getBuildList()) { p = q; break; }
      }
    }
    if (!p || !p->getBuildList())
      return "{\"accepted\":false,\"reason\":\"no build list to reorder\"}";

    // 3) Work on a fresh duplicate (original order); collect its nodes.
    BuildListInfo* clone = p->getBuildList()->duplicate();
    std::vector<BuildListInfo*> nodes;
    for (BuildListInfo* n = clone; n; n = n->getNext()) nodes.push_back(n);

    // 4) Match requested order against clone nodes (first-unused wins). Requested
    //    items with no matching node are dropped (faction can't build them).
    std::vector<char> usedFlag(nodes.size(), 0);
    std::vector<BuildListInfo*> neworder;
    for (const AsciiString& want : order) {
      for (size_t i = 0; i < nodes.size(); ++i) {
        if (usedFlag[i]) continue;
        if (templateMatchesRequest(nodes[i]->getTemplateName().str(), want)) {
          usedFlag[i] = 1;
          neworder.push_back(nodes[i]);
          break;
        }
      }
    }
    const Int matched = (Int)neworder.size();

    // 4b) Coverage, not silence: if NONE of the requested items matched anything
    //     in the build list, don't report a fake success — the agent has no way
    //     to tell "reorder applied" from "nothing you asked for exists" otherwise.
    //     Discard the untouched clone and leave the player's real list alone.
    if (matched == 0) {
      deleteInstance(clone);
      return "{\"accepted\":false,\"reason\":\"no requested items matched the build list\"}";
    }

    // Keep every clone node: append the unmatched ones (original relative order)
    // so the AI retains a complete, valid plan — matched items simply move to the
    // front. Dropping nodes here would risk desync; prioritizing does not.
    for (size_t i = 0; i < nodes.size(); ++i)
      if (!usedFlag[i]) neworder.push_back(nodes[i]);

    // 5) Relink the clone into the new order and install it (deletes the original).
    for (size_t i = 0; i + 1 < neworder.size(); ++i)
      neworder[i]->setNextBuildList(neworder[i + 1]);
    if (!neworder.empty()) neworder.back()->setNextBuildList(NULL);
    p->setBuildList(neworder.empty() ? NULL : neworder[0]);

    // 6) Derive applied_order by READING BACK the actual mutated list head (the
    //    first `matched` nodes), so the echo proves the mutation, not the request.
    AsciiString result = "{\"accepted\":true,\"effect\":\"priority_changed\",\"applied_order\":[";
    Bool first = TRUE;
    Int emitted = 0;
    for (BuildListInfo* n = p->getBuildList(); n && emitted < matched;
         n = n->getNext(), ++emitted) {
      const char* tmpl = n->getTemplateName().str();
      const char* logi = logicalForTemplate(tmpl);
      if (!first) result.concat(',');
      first = FALSE;
      result.concat('"');
      jsonEscape(result, logi ? logi : tmpl);
      result.concat('"');
    }
    result.concat("]}");
    return result;
  }

  if (op == "set_team_priorities") {
    // 1) Parse args.teams[] depth/scope-aware: locate "args", brace-match its
    //    extent, and read "teams" (an array of {unit,priority,count?} objects)
    //    only from WITHIN those bounds.
    std::vector<TeamPriorityReq> teams;
    const char* argsVal = jsonFindValue(req, "args");
    if (argsVal && *argsVal == '{') {
      const char* argsEnd = jsonMatchBracket(argsVal);
      if (argsEnd) jsonReadTeamsArrayIn(argsVal, argsEnd, teams);
    }
    if (teams.empty())
      return "{\"accepted\":false,\"reason\":\"missing or empty args.teams[]\"}";

    if (!ThePlayerList)
      return "{\"accepted\":false,\"reason\":\"no player list\"}";

    // 2) Pick the target player. AUTHORITATIVE on m_bridgePlayerIndex (Phase 4.2):
    //    when the bridge player is bound, act on THAT player only — never fall
    //    back to "first team-owning player", so the op steers exactly the player
    //    observe() reports (C4). The fallback scan (first player with actual
    //    PRODUCTION team prototypes; see hasProductionTeams()'s comment on why a
    //    raw non-empty getPlayerTeams() is not a useful signal) runs only when the
    //    bridge player is unset (m_bridgePlayerIndex < 0).
    Player* p = NULL;
    if (m_bridgePlayerIndex >= 0) {
      p = ThePlayerList->getNthPlayer(m_bridgePlayerIndex);
    } else {
      for (Int pi = 0; pi < ThePlayerList->getPlayerCount(); ++pi) {
        Player* q = ThePlayerList->getNthPlayer(pi);
        if (hasProductionTeams(q)) { p = q; break; }
      }
    }
    if (!p || !hasProductionTeams(p))
      return "{\"accepted\":false,\"reason\":\"no team prototypes to prioritize\"}";

    // 3) For each requested {unit,priority}, match against EVERY team
    //    prototype's unit-slot templates (a team is a composition of up to
    //    MAX_UNIT_TYPES unit types, not a single template -- so a request can
    //    legitimately touch more than one prototype, e.g. "tomahawk" matching
    //    both a pure-Tomahawk team and a mixed escort team). `count` is parsed
    //    but not applied in v1: it would mean changing team COMPOSITION
    //    (m_unitsInfo[].minUnits/maxUnits), not priority, and is deferred like
    //    kLogicalMap's per-faction table.
    std::vector<TeamPrototype*> touched;   // dedup, first-seen order (for readback)
    Int mutatedCount = 0;
    for (const TeamPriorityReq& te : teams) {
      for (TeamPrototype* proto : *p->getPlayerTeams()) {
        if (!proto) continue;
        const TeamTemplateInfo* info = proto->getTemplateInfo();
        Bool matches = FALSE;
        const Int n = (info->m_numUnitsInfo < (Int)TeamTemplateInfo::MAX_UNIT_TYPES)
                        ? info->m_numUnitsInfo : (Int)TeamTemplateInfo::MAX_UNIT_TYPES;
        for (Int i = 0; i < n; ++i) {
          if (unitTemplateMatchesRequest(info->m_unitsInfo[i].unitThingName.str(), te.unit)) {
            matches = TRUE;
            break;
          }
        }
        if (!matches) continue;
        // m_productionPriority is `mutable Int` (Phase 0 proved this safe);
        // const_cast per the task brief for clarity at the call site.
        const_cast<TeamTemplateInfo*>(info)->m_productionPriority = te.priority;
        ++mutatedCount;
        Bool already = FALSE;
        for (TeamPrototype* t : touched) if (t == proto) { already = TRUE; break; }
        if (!already) touched.push_back(proto);
      }
    }

    // 4) Coverage, not silence: if NOTHING requested matched ANY team
    //    prototype, report accepted:false rather than a fake success --
    //    nothing was mutated (mirrors set_build_list's matched==0 case).
    if (mutatedCount == 0)
      return "{\"accepted\":false,\"reason\":\"no requested units matched any team\"}";

    // 5) active_teams is read back from the ACTUAL mutated prototypes (name +
    //    resulting priority), proving the change rather than echoing the ask.
    AsciiString result = "{\"accepted\":true,\"effect\":\"priority_changed\",\"active_teams\":[";
    Bool first = TRUE;
    for (TeamPrototype* proto : touched) {
      if (!first) result.concat(',');
      first = FALSE;
      result.concat("{\"name\":\"");
      jsonEscape(result, proto->getName().str());
      AsciiString tmp;
      tmp.format("\",\"priority\":%d}", proto->getTemplateInfo()->m_productionPriority);
      result.concat(tmp);
    }
    result.concat("]}");
    return result;
  }

  if (op == "attack") {
    // spec A.4: order the bridge player's combat teams to attack-move to a named
    // region's center. Fog only ANNOTATES the reply (a warning) — it NEVER gates
    // the order. Rejection is resource-only (no combat units) or unknown-region.

    // 1) Parse args.target_region (+ optional commit) depth/scope-aware.
    AsciiString targetRegion, commit;
    const char* argsVal = jsonFindValue(req, "args");
    if (argsVal && *argsVal == '{') {
      const char* argsEnd = jsonMatchBracket(argsVal);
      if (argsEnd) {
        jsonReadStrIn(argsVal, argsEnd, "target_region", targetRegion);
        jsonReadStrIn(argsVal, argsEnd, "commit", commit);   // "all"|"ready_only"; see note below
      }
    }
    if (targetRegion.isEmpty())
      return "{\"accepted\":false,\"reason\":\"missing args.target_region\"}";

    // 2) Resolve the region -> its center Coord3D (regions come from Task 1.1's
    //    waypoint/polygon derivation). Unknown name is a hard reject.
    // Task 5: resolveMyStart() used to run only from observe(), so "my_base"/
    // "enemy_start_N" aliases were unresolved (and refused as unknown-region)
    // until an observe had been sent at least once. Resolve here too, before
    // regionByName, so the very first order can use them. Idempotent and safe
    // when the bridge player's command center doesn't exist yet (no-op).
    if (m_bridgePlayerIndex >= 0 && ThePlayerList)
      resolveMyStart(ThePlayerList->getNthPlayer(m_bridgePlayerIndex));
    const BridgeRegion* region = regionByName(targetRegion);
    if (!region)
      return "{\"accepted\":false,\"reason\":\"unknown region\"}";
    const Coord3D center = region->center;

    if (!ThePlayerList)
      return "{\"accepted\":false,\"reason\":\"no player list\"}";

    // 3) Pick the player to command. AUTHORITATIVE on m_bridgePlayerIndex (Phase
    //    4.2): when the bridge player is bound, command ONLY that player's own
    //    attack-role units — never fall back to another player. This is exactly
    //    what C4 requires: fog is per-player, so the attack must move the bridge
    //    player's OWN units (moving a foreign player's units would reveal nothing
    //    in the bridge player's observe()). The fallback scan (first player owning
    //    attack-role units) runs only when the bridge player is unset (< 0).
    Player* p = NULL;
    CombatAccum acc;
    if (m_bridgePlayerIndex >= 0) {
      p = ThePlayerList->getNthPlayer(m_bridgePlayerIndex);
      if (p) p->iterateObjects(combatUnitCb, &acc);
    } else {
      for (Int pi = 0; pi < ThePlayerList->getPlayerCount(); ++pi) {
        Player* q = ThePlayerList->getNthPlayer(pi);
        if (!q) continue;
        acc.units.clear();
        q->iterateObjects(combatUnitCb, &acc);
        if (!acc.units.empty()) { p = q; break; }
      }
    }
    // Resource-only rejection: nothing to send (the bridge player's army may not
    // be up yet headless — see concern C1 — but we never command a foreign slot).
    if (!p || acc.units.empty())
      return "{\"accepted\":false,\"reason\":\"no attack-role teams available\"}";

    const Int idx = p->getPlayerIndex();

    // 4) Issue the order. Mechanism = Option B (direct per-unit AI command). The
    //    spec's Option A (build a MSG_CREATE_SELECTED_GROUP + MSG_DO_ATTACKMOVETO
    //    human-input mirror) routes through client-side selection state that does
    //    not exist headless (no InGameUI selection); commanding each unit's
    //    AICommandInterface directly is the mechanism that actually issues a
    //    verifiable order with no UI. aiAttackMoveToPosition mirrors the human
    //    attack-move (move toward the point, engage what it meets), and we pass
    //    CMD_FROM_PLAYER (not CMD_FROM_AI) precisely to preserve that human-mirror
    //    semantics at the AIUpdate layer: CMD_FROM_PLAYER is what gates the
    //    forbidPlayerCommands check (AIUpdate.cpp:2610, so units locked out of
    //    player control correctly reject this order the same as they would a real
    //    player order) and what triggers setGoalPositionClipped (AIUpdate.cpp:355-
    //    360, so the goal position gets clamped to the map instead of an AI-only
    //    order being allowed to target off-map points). This is a command-source
    //    tag only; it does not require any UI/MessageStream plumbing. `commit`
    //    (all vs ready_only) is parsed but not yet differentiated in v1: with no
    //    per-team readiness model headless we commit every attack-role unit we
    //    found; refining ready_only to skip in-production/garrisoned teams is
    //    deferred like the other v1 heuristics.
    Int committed = 0;
    for (Object* u : acc.units) {
      AIUpdateInterface* ai = u->getAIUpdateInterface();
      if (!ai) continue;
      ai->aiAttackMoveToPosition(&center, NO_MAX_SHOTS_LIMIT, CMD_FROM_PLAYER);
      ++committed;
    }

    // 5) FOG = WARN, NEVER BLOCK. Read the SAME shroud query that fogs observe(),
    //    now used only to annotate: if the target cell is not clear for the
    //    commanding player, the order STILL went through — we just flag it.
    Bool fogged = FALSE;
    if (ThePartitionManager) {
      CellShroudStatus s = ThePartitionManager->getShroudStatusForPlayer(idx, &center);
      if (s != CELLSHROUD_CLEAR) fogged = TRUE;
    }

    fprintf(stderr, "[BRIDGE] attack: committed %d unit(s) of player %d to region '%s'%s\n",
            committed, idx, targetRegion.str(), fogged ? " (into fog)" : "");
    fflush(stderr);

    // 6) Result. teams_committed = the number of units we actually ordered. Echo
    //    the REQUESTED name (Task 8), not region->name, so an alias like
    //    "enemy_start_1" comes back as itself rather than the underlying
    //    "Player_N_Start" waypoint name.
    AsciiString result = "{\"accepted\":true,\"effect\":\"units_ordered\",\"teams_committed\":";
    AsciiString num; num.format("%d", committed);
    result.concat(num);
    result.concat(",\"target\":\"");
    jsonEscape(result, targetRegion.str());
    result.concat("\"");
    if (fogged)
      result.concat(",\"warning\":\"target_region not currently visible \\u2014 "
                    "committing into fog; enemy strength there is unknown\"");
    result.concat("}");
    return result;
  }

  if (op == "scout") {
    // spec A.5: send ONE cheap/mobile unit on a plain MOVE (not attack-move) to
    // a named region's center, to buy sight vs fog. No fog check needed here --
    // scouting IS how you clear fog, unlike `attack` which annotates fog as a
    // warning.

    // 1) Parse args.region depth/scope-aware.
    AsciiString regionName;
    const char* argsVal = jsonFindValue(req, "args");
    if (argsVal && *argsVal == '{') {
      const char* argsEnd = jsonMatchBracket(argsVal);
      if (argsEnd) jsonReadStrIn(argsVal, argsEnd, "region", regionName);
    }
    if (regionName.isEmpty())
      return "{\"accepted\":false,\"reason\":\"missing args.region\"}";

    // 2) Resolve the region -> its center Coord3D. Unknown name is a hard reject.
    // Task 5: resolve "my_base"/"enemy_start_N" before the first observe too --
    // see the matching comment in the `attack` branch above.
    if (m_bridgePlayerIndex >= 0 && ThePlayerList)
      resolveMyStart(ThePlayerList->getNthPlayer(m_bridgePlayerIndex));
    const BridgeRegion* region = regionByName(regionName);
    if (!region)
      return "{\"accepted\":false,\"reason\":\"unknown region\"}";
    const Coord3D center = region->center;

    if (!ThePlayerList)
      return "{\"accepted\":false,\"reason\":\"no player list\"}";

    // 3) Pick the player to scout with. AUTHORITATIVE on m_bridgePlayerIndex
    //    (Phase 4.2): when the bridge player is bound, dispatch ONLY one of that
    //    player's own mobile units — never fall back to another player. Fog is
    //    per-player (C4): the whole point of scouting is to clear the BRIDGE
    //    player's fog, so the moving unit must be the bridge player's own. The
    //    fallback scan (first player owning a scoutable unit) runs only when the
    //    bridge player is unset (< 0).
    ScoutAccum acc; acc.unit = NULL;
    if (m_bridgePlayerIndex >= 0) {
      Player* bp = ThePlayerList->getNthPlayer(m_bridgePlayerIndex);
      if (bp) bp->iterateObjects(scoutUnitCb, &acc);
    } else {
      for (Int pi = 0; pi < ThePlayerList->getPlayerCount(); ++pi) {
        Player* q = ThePlayerList->getNthPlayer(pi);
        if (!q) continue;
        q->iterateObjects(scoutUnitCb, &acc);
        if (acc.unit) break;
      }
    }
    // Resource-only rejection: nothing to send (the bridge player's army may not
    // be up yet headless — see concern C1 — but we never command a foreign slot).
    if (!acc.unit)
      return "{\"accepted\":false,\"reason\":\"no scoutable unit available\"}";

    // 4) Issue a plain MOVE (not attack-move) toward the region center.
    //    CMD_FROM_PLAYER for the same reason as `attack`: it gates
    //    forbidPlayerCommands and clips the goal position to the map (human-
    //    mirror semantics), matching the fix already applied to the attack op.
    AIUpdateInterface* ai = acc.unit->getAIUpdateInterface();
    ai->aiMoveToPosition(&center, CMD_FROM_PLAYER);

    const ThingTemplate* tt = acc.unit->getTemplate();
    const char* tmplName = tt ? tt->getName().str() : "";

    fprintf(stderr, "[BRIDGE] scout: dispatched '%s' to region '%s'\n",
            tmplName, regionName.str());
    fflush(stderr);

    // 5) Result. Echo the REQUESTED name (Task 8), not region->name, so an
    //    alias like "enemy_start_1" comes back as itself.
    AsciiString result = "{\"accepted\":true,\"effect\":\"units_ordered\",\"scout_dispatched\":\"";
    jsonEscape(result, tmplName);
    result.concat("\",\"region\":\"");
    jsonEscape(result, regionName.str());
    result.concat("\"}");
    return result;
  }

  if (op == "build_now" || op == "train_now") {
    // Direct ops (Task 9): unlike set_build_list/set_team_priorities (which only
    // nudge a PRIORITY the AI acts on at its own pace), these call the same
    // public script-engine forwarders the map script engine uses
    // (Player::buildSpecificBuilding / Player::buildSpecificTeam) to tell the
    // bridge player's AI to start this specific thing as soon as it can.
    const Bool building = op == "build_now";
    AsciiString want;
    const char* argsVal = jsonFindValue(req, "args");
    if (argsVal && *argsVal == '{') {
      const char* argsEnd = jsonMatchBracket(argsVal);
      if (argsEnd) jsonReadStrIn(argsVal, argsEnd, building ? "structure" : "unit", want);
    }
    if (want.isEmpty())
      return building ? "{\"accepted\":false,\"reason\":\"missing args.structure\"}"
                      : "{\"accepted\":false,\"reason\":\"missing args.unit\"}";
    Player* p = (ThePlayerList && m_bridgePlayerIndex >= 0) ? ThePlayerList->getNthPlayer(m_bridgePlayerIndex) : NULL;
    if (!p) return "{\"accepted\":false,\"reason\":\"bridge player not bound yet\"}";

    AsciiString result;
    if (building) {
      // Finding A: mirror AISkirmishPlayer::buildSpecificAIBuilding's OWN precondition
      // (AISkirmishPlayer.cpp:409-445) before calling it, instead of always claiming
      // "requested_now". That function marks only the first build-list entry that is
      // BOTH not-yet-built (no live Object for its BuildListInfo::getObjectID()) AND not
      // already flagged isPriorityBuild(); if every matching entry fails that test it logs
      // "already built or queued" and does nothing -- silently. Find such an entry
      // ourselves so we can tell the caller apart from a real accept.
      AsciiString tmpl;
      Bool anyMatch = FALSE;
      for (BuildListInfo* n = p->getBuildList(); n; n = n->getNext()) {
        if (!templateMatchesRequest(n->getTemplateName().str(), want)) continue;
        anyMatch = TRUE;
        if (TheGameLogic->findObjectByID(n->getObjectID())) continue;   // already built
        if (n->isPriorityBuild())                          continue;   // already queued
        tmpl = n->getTemplateName();
        break;
      }
      if (tmpl.isEmpty()) {
        result = "{\"accepted\":false,\"reason\":\"";
        if (!anyMatch) {
          jsonEscape(result, want.str());
          result.concat(" is not in this faction's build list\"}");
        } else {
          result.concat("every ");
          jsonEscape(result, want.str());
          result.concat(" in the build list is already built or queued\"}");
        }
        return result;
      }
      p->buildSpecificBuilding(tmpl);
      result = "{\"accepted\":true,\"effect\":\"requested_now\",\"template\":\""; jsonEscape(result, tmpl.str()); result.concat("\"}");
    } else {
      TeamPrototype* hit = NULL;
      for (TeamPrototype* proto : *p->getPlayerTeams()) {
        const TeamTemplateInfo* ti = proto ? proto->getTemplateInfo() : NULL;
        if (!ti) continue;
        for (Int u = 0; u < ti->m_numUnitsInfo && !hit; ++u)
          if (unitTemplateMatchesRequest(ti->m_unitsInfo[u].unitThingName.str(), want)) hit = proto;
        if (hit) break;
      }
      if (!hit) {
        result = "{\"accepted\":false,\"reason\":\"no team trains "; jsonEscape(result, want.str()); result.concat("\"}");
        return result;
      }

      // Finding A: mirror AIPlayer::buildSpecificAITeam's preconditions (AIPlayer.cpp:
      // 2435-2477) before calling Player::buildSpecificTeam(hit) (which always does a
      // priorityBuild=true call, so it queues unconditionally unless we refuse first).
      // We reproduce ONLY the checks that make it return without queueing (the ones that
      // don't touch protected AIPlayer state and have no side effects to evaluate):
      //   1) getCanBuildUnits() must be true.
      //   2) a singleton team whose Team instance already has objects can't be re-queued.
      //   3) isPossibleToBuildTeam(hit, requireIdleFactory=false, needMoney) — its ONLY
      //      false-returning branch reachable with requireIdleFactory=false is "some unit
      //      type in the team has no factory at all" (findFactory(thing,true)==NULL); the
      //      not-enough-money branch still queues (buildSpecificAITeam queues anyway and
      //      just logs a note), so it is not a rejection case and is deliberately not
      //      reproduced here.
      const TeamTemplateInfo* ti = hit->getTemplateInfo();
      const char* why = NULL;
      if (!p->getCanBuildUnits()) {
        why = "unit building is disabled for this player";
      } else if (hit->getIsSingleton()) {
        Team* singleton = TheTeamFactory ? TheTeamFactory->findTeam(hit->getName()) : NULL;
        if (singleton && singleton->hasAnyObjects()) why = "singleton team already exists";
      }
      if (!why) {
        const Int n = (ti->m_numUnitsInfo < (Int)TeamTemplateInfo::MAX_UNIT_TYPES)
                        ? ti->m_numUnitsInfo : (Int)TeamTemplateInfo::MAX_UNIT_TYPES;
        for (Int u = 0; u < n; ++u) {
          const ThingTemplate* thing = TheThingFactory ? TheThingFactory->findTemplate(ti->m_unitsInfo[u].unitThingName) : NULL;
          if (!thing) continue;
          if (!hasFactoryForThing(p, thing)) { why = "required factories/tech missing"; break; }
        }
      }
      if (why) {
        result = "{\"accepted\":false,\"reason\":\"cannot train ";
        jsonEscape(result, hit->getName().str());
        result.concat(" now: ");
        jsonEscape(result, why);
        result.concat("\"}");
        return result;
      }

      p->buildSpecificTeam(hit);
      result = "{\"accepted\":true,\"effect\":\"requested_now\",\"team\":\""; jsonEscape(result, hit->getName().str()); result.concat("\"}");
    }
    return result;
  }

  return "{\"accepted\":false,\"reason\":\"unknown apply op\"}";
}

const BridgeRegion* ControlBridge::regionByName(const AsciiString& n) const
{
  // Task 8: semantic aliases resolve first ("my_base", "enemy_start_N"), ahead
  // of the raw waypoint/trigger lookup.
  if (n == "my_base") return m_myStart >= 0 ? &m_starts[m_myStart] : NULL;
  if (n.startsWith("enemy_start_")) {
    // Fix round 1 (Important #1): until my_base resolves we don't know which
    // start is ours, so no start can be safely labeled an enemy's yet.
    if (m_myStart < 0) return NULL;
    if (!isAllDigits(n.str() + 12)) return NULL;   // reject "enemy_start_1abc" etc.
    Int want = atoi(n.str() + 12), e = 0;
    for (size_t i = 0; i < m_starts.size(); ++i) {
      if ((Int)i == m_myStart) continue;
      if (++e == want) return &m_starts[i];
    }
    return NULL;
  }
  for (const BridgeRegion& r : m_regions) if (r.name == n) return &r;
  return NULL;
}
