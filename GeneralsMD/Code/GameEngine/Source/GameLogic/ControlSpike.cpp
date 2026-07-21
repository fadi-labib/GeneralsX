// SPDX-License-Identifier: GPL-3.0-only
// ControlSpike.cpp — TEMPORARY Phase-0 spike. Deleted in Task 0.4.
#include "PreRTS.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Team.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/SidesList.h"   // BuildListInfo

// Find a computer-controlled skirmish player to mutate.
// NOTE (deviation from brief): the brief returns the *first* skirmish-AI player, but in
// a live skirmish several slots read as skirmish-AI and only one has a populated build
// list (the others are empty/neutral). To make the build-list reorder observable we
// prefer the first skirmish-AI player whose build list is non-empty, falling back to the
// first skirmish-AI player otherwise.
static Player *findAIPlayer(void)
{
  Player *firstAI = NULL;
  for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i) {
    Player *p = ThePlayerList->getNthPlayer(i);
    if (p && p->isSkirmishAIPlayer()) {
      if (firstAI == NULL) firstAI = p;
      if (p->getBuildList() != NULL) return p;   // prefer an AI with a real build list
    }
  }
  return firstAI;
}

void control_spike_tick(void)
{
  if (TheGameLogic == NULL) return;

  static bool s_fired = false;
  static Player *s_ai = NULL;

  // Step-6 monitor: after firing, periodically report whether the AI is consuming the
  // reordered build list (head entry gets an ObjectID once the AI builds it) and its
  // building count. Lines are prefixed "warn" so the dev harness stderr filter keeps them.
  if (s_fired) {
    if (s_ai == NULL) return;
    static UnsignedInt s_next = 0;
    UnsignedInt f = TheGameLogic->getFrame();
    if (f >= s_next) {
      s_next = f + 300;   // ~10s cadence
      BuildListInfo *h = s_ai->getBuildList();
      fprintf(stderr, "[SPIKE-MON] warn frame=%u head=%s headObjId=%d buildings=%d\n",
              f, h ? h->getTemplateName().str() : "NULL",
              h ? (int)h->getObjectID() : -1, s_ai->countBuildings());
      fflush(stderr);
    }
    return;
  }

  // Fire exactly once, at the first logic frame at/after 300 (~10s at 30fps).
  // NOTE (deviation from brief): the verbatim `getFrame() != 300` equality guard is
  // fragile in this wasm port — the browser rAF loop steps several logic frames per
  // callback (fixed-timestep catch-up), so getFrame() is only sampled once per rAF and
  // can step over exactly 300. A fire-once latch on frame >= 300 is robust to that.
  if (s_fired) return;
  if (TheGameLogic->getFrame() < 300) return;
  s_fired = true;

  Player *ai = findAIPlayer();
  if (ai == NULL) { fprintf(stderr, "[SPIKE] no AI player found\n"); return; }

  // (a) Move the LAST build-list entry to the FRONT (a visible reorder).
  BuildListInfo *head = ai->getBuildList();
  if (head && head->getNext()) {
    fprintf(stderr, "[SPIKE] build list old head=%s\n", head->getTemplateName().str());
    BuildListInfo *prev = head, *cur = head->getNext();
    while (cur->getNext()) { prev = cur; cur = cur->getNext(); }   // cur = last
    prev->setNextBuildList(NULL);                                  // detach last
    cur->setNextBuildList(head);                                   // last -> old head
    ai->setBuildList(cur);                                         // new head = last
    fprintf(stderr, "[SPIKE] build list reordered; new head=%s\n",
            cur->getTemplateName().str());
  } else {
    fprintf(stderr, "[SPIKE] build list has <2 entries; no reorder (head=%s)\n",
            head ? head->getTemplateName().str() : "NULL");
  }

  // (b) Bump every team prototype's production priority by +5.
  const Player::PlayerTeamList *teams = ai->getPlayerTeams();
  for (Player::PlayerTeamList::const_iterator t = teams->begin(); t != teams->end(); ++t) {
    TeamTemplateInfo *info = const_cast<TeamTemplateInfo*>((*t)->getTemplateInfo());
    info->m_productionPriority += 5;
  }
  fprintf(stderr, "[SPIKE] bumped %zu team priorities\n", teams->size());
  fflush(stderr);
  s_ai = ai;   // hand off to the Step-6 monitor above
}
