// GeneralsX @build dx8wasm - link stubs for builds WITHOUT SAGE_USE_NGMP (the browser build).
//
// The online-lobby menus (WOLLobbyMenu, WOLMapSelectMenu, WOLGameSetupMenu) call into NGMP
// without a SAGE_USE_NGMP guard, but the NGMP sources (and libcurl) are only built when it is on.
// On the web those menus are unreachable: the main menu's Online entry is hidden and its handler
// swallows clicks. These definitions let the engine link. TheNGMPGame stays null, which every
// caller already checks; the functions report what was reached and abort, because silently
// pretending to be online would hide a real routing bug.
#include "PreRTS.h"

#include "GameNetwork/GeneralsOnline/NGMPGame.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Manager.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_LobbyInterface.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_RoomsInterface.h"

#include <cstdio>
#include <cstdlib>

NGMPGame* TheNGMPGame = nullptr;

[[noreturn]] static void ngmpDisabled(const char* what)
{
	fprintf(stderr, "[NGMP] %s reached in a build without NGMP (SAGE_USE_NGMP off)\n", what);
	fflush(stderr);
	abort();
}

NGMP_OnlineServicesManager& NGMP_OnlineServicesManager::getInstance()
{
	ngmpDisabled("NGMP_OnlineServicesManager::getInstance");
}

void NGMP_OnlineServices_LobbyInterface::LeaveCurrentLobby()
{
	ngmpDisabled("NGMP_OnlineServices_LobbyInterface::LeaveCurrentLobby");
}

void NGMP_OnlineServices_RoomsInterface::SendChatMessageToCurrentRoom(UnicodeString&, bool)
{
	ngmpDisabled("NGMP_OnlineServices_RoomsInterface::SendChatMessageToCurrentRoom");
}

void WebSocket::SendData_ChangeName(UnicodeString&)
{
	ngmpDisabled("WebSocket::SendData_ChangeName");
}

void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_Map(AsciiString, AsciiString, bool, int)
{
	ngmpDisabled("NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_Map");
}

NGMPGameSlot* NGMPGame::getGameSpySlot(Int)
{
	ngmpDisabled("NGMPGame::getGameSpySlot");
}
