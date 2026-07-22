// SPDX-License-Identifier: GPL-3.0-only
#include "PreRTS.h"
#include "GameLogic/ControlBridge.h"
#include "GameLogic/TerrainLogic.h"     // TheTerrainLogic, Waypoint
#include "GameLogic/PolygonTrigger.h"
#include "GameLogic/SidesList.h"        // BuildListInfo (build-list nodes)
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
static const LogicalMap kLogicalMap[] = {
  { "power_plant",    "PowerPlant ColdFusion Reactor" },
  { "supply_center",  "SupplyCenter SupplyStash SupplyDepot" },
  { "barracks",       "Barracks" },
  { "war_factory",    "WarFactory ArmsDealer" },
  { "airfield",       "Airfield Helipad" },
  { "defense_turret", "Gatling Stinger Firebase Bunker Patriot Tunnel" },
  { "superweapon",    "ParticleCannon MissileSilo ScudStorm Nuke" },
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

  // Dispatch. `observe` is the read-op; `set_build_list` is the first write-op
  // (Phase 3, routed through apply()); every other op still gets an explicit
  // "not implemented" reply (coverage, never a silent drop).
  AsciiString result;
  if (op == "observe") {
    Int who = (m_bridgePlayerIndex >= 0) ? m_bridgePlayerIndex : 0;
    result = observe(who);
    if (result.isEmpty()) result = "{}";
  } else if (op == "set_build_list") {
    result = apply(op, buf);
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

    // 2) Pick a build-list-owning player (bridge player first, else first AI).
    Player* p = NULL;
    if (m_bridgePlayerIndex >= 0) {
      Player* bp = ThePlayerList->getNthPlayer(m_bridgePlayerIndex);
      if (bp && bp->getBuildList()) p = bp;
    }
    if (!p) {
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

  return "{\"accepted\":false,\"reason\":\"unknown apply op\"}";
}

const BridgeRegion* ControlBridge::regionByName(const AsciiString& n) const
{
  for (const BridgeRegion& r : m_regions) if (r.name == n) return &r;
  return NULL;
}
