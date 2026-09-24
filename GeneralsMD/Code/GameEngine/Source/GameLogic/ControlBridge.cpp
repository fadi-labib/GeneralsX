// SPDX-License-Identifier: GPL-3.0-only
#include "PreRTS.h"
#include "GameLogic/ControlBridge.h"
#include "GameLogic/TerrainLogic.h"     // TheTerrainLogic, Waypoint
#include "GameLogic/PolygonTrigger.h"
#include "GameLogic/SidesList.h"        // BuildListInfo (build-list nodes)
#include "GameLogic/GameLogic.h"        // TheGameLogic
#include "GameLogic/Object.h"           // Object, getShroudedStatus/getTemplate/isKindOf
#include "GameLogic/Module/AIUpdate.h"  // AIUpdateInterface::aiAttackMoveToPosition (attack order)
#include "GameLogic/Weapon.h"           // NO_MAX_SHOTS_LIMIT
#include "GameLogic/PartitionManager.h" // ThePartitionManager, getShroudStatusForPlayer, CellShroudStatus
#include "Common/GameCommon.h"          // CMD_FROM_AI, CELLSHROUD_CLEAR
#include "Common/GlobalData.h"          // TheGlobalData->m_wasmBridgeSide (Task 4.3)
#include "Common/Player.h"              // Player, Money, Energy
#include "Common/PlayerList.h"          // ThePlayerList
#include "Common/Team.h"                // ObjectIterateFunc, Team
#include "Common/ThingTemplate.h"       // ThingTemplate::getName
#include "Common/KindOf.h"              // KINDOF_*
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
      Int who = (m_bridgePlayerIndex >= 0) ? m_bridgePlayerIndex : 0;
      result = observe(who);
      if (result.isEmpty()) result = "{}";
    } else if (op == "set_build_list" || op == "set_team_priorities" || op == "attack" || op == "scout") {
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
    AsciiString result = "{\"accepted\":true,\"applied_order\":[";
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
    AsciiString result = "{\"accepted\":true,\"active_teams\":[";
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
            committed, idx, region->name.str(), fogged ? " (into fog)" : "");
    fflush(stderr);

    // 6) Result. teams_committed = the number of units we actually ordered.
    AsciiString result = "{\"accepted\":true,\"teams_committed\":";
    AsciiString num; num.format("%d", committed);
    result.concat(num);
    result.concat(",\"target\":\"");
    jsonEscape(result, region->name.str());
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
            tmplName, region->name.str());
    fflush(stderr);

    // 5) Result.
    AsciiString result = "{\"accepted\":true,\"scout_dispatched\":\"";
    jsonEscape(result, tmplName);
    result.concat("\",\"region\":\"");
    jsonEscape(result, region->name.str());
    result.concat("\"}");
    return result;
  }

  return "{\"accepted\":false,\"reason\":\"unknown apply op\"}";
}

const BridgeRegion* ControlBridge::regionByName(const AsciiString& n) const
{
  for (const BridgeRegion& r : m_regions) if (r.name == n) return &r;
  return NULL;
}
