/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: GameClient.cpp ////////////////////////////////////////////////////
// Implementation of GameClient singleton
// Author: Michael S. Booth, March 2001
///////////////////////////////////////////////////////////////////////////////

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
#include <stdexcept>
#include <cstdio>
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "GameClient/GameClient.h"

// GeneralsX @build dx8wasm --- phase breakdown of frame.client -------------------------------
// frame.client wraps this whole function (GameEngine.cpp). Once GLBuffer::Unlock stopped
// respecifying whole buffers, the frame fell from 24.96 ms to 5.43 ms and the SDK's gl.* gauges --
// which had accounted for 98.5% of the old frame -- accounted for only 53%. The other ~2.5 ms is
// engine CPU work inside this function that nothing has ever timed. It did not grow; removing the
// dominant term made it visible.
//
// AGGREGATED, NOT PER-FRAME, and that is not a detail. GameEngine.cpp's budget note reasons from
// "two spans plus one gauge a frame at 60 FPS is 180 records/second" against a 1024-record ring --
// a premise the 4.6x made stale: at the 184 fps now measured those same three records are 552/s.
// Emitting eleven more PER FRAME would be ~2000/s and would destroy the telemetry it reports
// through, which has already happened once in this project (IM-07). Per-second averages cost 11
// records/second instead.
//
// client.accounted_ms is the instrument's own completeness check: frame.client minus it is the
// part still un-timed. An instrument that cannot report what it is missing invites its residual
// being read as zero.
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <dx8wasm/telemetry.h>
namespace {
double g_cAccFrames = 0.0;
double g_cInput = 0.0, g_cWindow = 0.0, g_cVideo = 0.0, g_cDrawables = 0.0, g_cTerrain = 0.0;
double g_cDispUpd = 0.0, g_cDispDraw = 0.0, g_cStrings = 0.0, g_cShell = 0.0, g_cUI = 0.0;
double g_cLastEmit = 0.0;
// Doubles throughout, never cast to a 32-bit integer: a saturating fptoui silently killed the
// telemetry pump in this project once (dx8wasm runtime/telemetry/telemetry.cpp, now_ms()).
struct GxPhase {
	double &acc, t0;
	explicit GxPhase(double &a) : acc(a), t0(emscripten_get_now()) {}
	~GxPhase() { acc += emscripten_get_now() - t0; }
};

// Counts the frame and emits, from a DESTRUCTOR, so that every exit path from update() is counted
// -- including the intro/movie early return. The first version of this instrument counted only the
// full path while frame.client counted every call, so the two had different denominators and the
// per-frame phase averages came out inflated. client.accounted_ms then exceeded frame.client and
// the residual went NEGATIVE, which is impossible in a real accounting: the completeness check
// caught a bug in its own instrument. A residual that could only ever be positive would have shown
// a plausible ~94% and been believed.
struct GxFrameEnd {
	~GxFrameEnd();
};
}  // namespace
#define GX_PHASE(acc) GxPhase gx_phase_scope_(acc)
#define GX_NOW() emscripten_get_now()
#define GX_ADD(acc, t0) (acc) += emscripten_get_now() - (t0)
#define GX_FRAME_END() GxFrameEnd gx_frame_end_scope_
#else
#define GX_PHASE(acc) do {} while (0)
#define GX_NOW() 0.0
#define GX_ADD(acc, t0) do { (void)(t0); } while (0)
#define GX_FRAME_END() do {} while (0)
#endif

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Common/ActionManager.h"
#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/GameUtility.h"
#include "Common/GlobalData.h"
#include "Common/PerfTimer.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/Xfer.h"
#include "Common/GameLOD.h"
#include "GameClient/Anim2D.h"
#include "GameClient/CampaignManager.h"
#include "GameClient/ChallengeGenerals.h"
#include "GameClient/CommandXlat.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Diplomacy.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/Drawable.h"
#include "GameClient/DrawGroupInfo.h"
#include "GameClient/Eva.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/GraphDraw.h"
#include "GameClient/GUICommandTranslator.h"
#include "GameClient/HeaderTemplate.h"
#include "GameClient/HintSpy.h"
#include "GameClient/HotKey.h"
#include "GameClient/IMEManager.h"
#include "GameClient/InGameUI.h"
#include "GameClient/Keyboard.h"
#include "GameClient/LanguageFilter.h"
#include "GameClient/LookAtXlat.h"
#include "GameClient/MetaEvent.h"
#include "GameClient/Mouse.h"
#include "GameClient/ParticleSys.h"
#include "GameClient/PlaceEventTranslator.h"
#include "GameClient/RayEffect.h"
#include "GameClient/SelectionXlat.h"
#include "GameClient/Shell.h"
#include "GameClient/Snow.h"
#include "GameClient/TerrainVisual.h"
#include "GameClient/View.h"
#include "GameClient/VideoPlayer.h"
#include "GameClient/WindowXlat.h"
#include "GameLogic/FPUControl.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/GhostObject.h"
#include "GameLogic/Object.h"
#include "GameLogic/ScriptEngine.h"		// For TheScriptEngine - jkmcd

#define DRAWABLE_HASH_SIZE	8192

/// The GameClient singleton instance
GameClient *TheGameClient = nullptr;

//-------------------------------------------------------------------------------------------------
GameClient::GameClient()
{

	// zero our translator list
	for( Int i = 0; i < MAX_CLIENT_TRANSLATORS; i++ )
		m_translators[ i ] = TRANSLATOR_ID_INVALID;
	m_numTranslators = 0;
	m_commandTranslator = nullptr;

	m_drawableTOC.clear();
	m_textBearingDrawableList.clear();

	m_frame = 0;

	m_drawableList = nullptr;

	m_nextDrawableID = (DrawableID)1;
	TheDrawGroupInfo = new DrawGroupInfo;
}

//std::vector<std::string>	preloadTextureNamesGlobalHack;
//std::vector<std::string>	preloadTextureNamesGlobalHack2;

//-------------------------------------------------------------------------------------------------
GameClient::~GameClient()
{
#ifdef PERF_TIMERS
	delete TheGraphDraw;
	TheGraphDraw = nullptr;
#endif

	delete TheDrawGroupInfo;
	TheDrawGroupInfo = nullptr;

	// clear any drawable TOC we might have
	m_drawableTOC.clear();

	//DEBUG_LOG(("Preloaded texture files ------------------------------------------"));
	//for (Int oog=0; oog<preloadTextureNamesGlobalHack2.size(); ++oog)
	//{
	//	DEBUG_LOG(("%s", preloadTextureNamesGlobalHack2[oog]));
	//}
	//DEBUG_LOG(("------------------------------------------------------------------"));
	//for (oog=0; oog<preloadTextureNamesGlobalHack.size(); ++oog)
	//{
	//	DEBUG_LOG(("%s", preloadTextureNamesGlobalHack[oog]));
	//}
	//DEBUG_LOG(("End Texture files ------------------------------------------------"));

	delete TheCampaignManager;
	TheCampaignManager = nullptr;

	// destroy all Drawables
	Drawable *draw, *nextDraw;
	for( draw = m_drawableList; draw; draw = nextDraw )
	{
		nextDraw = draw->getNextDrawable();
		destroyDrawable( draw );
	}
	m_drawableList = nullptr;

	// delete the ray effects
	delete TheRayEffects;
	TheRayEffects = nullptr;

	// delete the hot key manager
	delete TheHotKeyManager;
	TheHotKeyManager = nullptr;

	// destroy the in-game user interface
	delete TheInGameUI;
	TheInGameUI = nullptr;

	delete TheChallengeGenerals;
	TheChallengeGenerals = nullptr;

	// delete the shell
	delete TheShell;
	TheShell = nullptr;

	delete TheIMEManager;
	TheIMEManager = nullptr;

	// delete window manager
	delete TheWindowManager;
	TheWindowManager = nullptr;

	// delete the font library
	TheFontLibrary->reset();
	delete TheFontLibrary;
	TheFontLibrary = nullptr;

	TheMouse->reset();
	delete TheMouse;
	TheMouse = nullptr;

	///@todo :  TheTerrainVisual used to be the first thing destroyed.
	//I had to put in here so that drawables free their track marks before
	//the terrain visual deletes the track laying system. MW

	// destroy the terrain visual representation
	delete TheTerrainVisual;
	TheTerrainVisual = nullptr;

	// destroy the display
	delete TheDisplay;
	TheDisplay = nullptr;

	delete TheHeaderTemplateManager;
	TheHeaderTemplateManager = nullptr;

	delete TheLanguageFilter;
	TheLanguageFilter = nullptr;

	delete TheVideoPlayer;
	TheVideoPlayer = nullptr;

	// destroy all translators
	for( UnsignedInt i = 0; i < m_numTranslators; i++ )
		TheMessageStream->removeTranslator( m_translators[ i ] );
	m_numTranslators = 0;
	m_commandTranslator = nullptr;

	delete TheAnim2DCollection;
	TheAnim2DCollection = nullptr;

	delete TheMappedImageCollection;
	TheMappedImageCollection = nullptr;

	delete TheKeyboard;
	TheKeyboard = nullptr;

	delete TheDisplayStringManager;
	TheDisplayStringManager = nullptr;

	delete TheEva;
	TheEva = nullptr;

	delete TheSnowManager;
	TheSnowManager = nullptr;

}

//-------------------------------------------------------------------------------------------------
/** Initialize resources for the game client */
//-------------------------------------------------------------------------------------------------
void GameClient::init()
{

	setFrameRate(MSEC_PER_LOGICFRAME_REAL);		// from GameCommon.h... tell W3D what our expected framerate is

	INI ini;
	// Load the DrawGroupInfo here, before the Display Manager is loaded.
	ini.loadFileDirectory("Data\\INI\\DrawGroupInfo", INI_LOAD_OVERWRITE, nullptr);

	// Override the ini values with localized versions:
	if (TheGlobalLanguageData && TheGlobalLanguageData->m_drawGroupInfoFont.name.isNotEmpty())
	{
		TheDrawGroupInfo->m_fontName = TheGlobalLanguageData->m_drawGroupInfoFont.name;
		TheDrawGroupInfo->m_fontSize = TheGlobalLanguageData->m_drawGroupInfoFont.size;
		TheDrawGroupInfo->m_fontIsBold = TheGlobalLanguageData->m_drawGroupInfoFont.bold;
	}

	// create the display string factory
	TheDisplayStringManager = createDisplayStringManager();
	if( TheDisplayStringManager )	{
		TheDisplayStringManager->init();
		TheDisplayStringManager->setName("TheDisplayStringManager");
	}

	if (!TheGlobalData->m_headless)
	{
		// create the keyboard
		TheKeyboard = createKeyboard();
		TheKeyboard->init();
		TheKeyboard->setName("TheKeyboard");
	}

	// allocate and load image collection for the GUI and just load the 256x256 ones for now
	TheMappedImageCollection = MSGNEW("GameClientSubsystem") ImageCollection;
	TheMappedImageCollection->load( 512 );

	// now that we have all the images loaded ... load any animation definitions from those images
	TheAnim2DCollection = MSGNEW("GameClientSubsystem") Anim2DCollection;
	TheAnim2DCollection->init();
 	TheAnim2DCollection->setName("TheAnim2DCollection");

	// register message translators
	if( TheMessageStream )
	{

		//
		// NOTE: Make sure m_translators[] is large enough to accommodate all the translators you
		// are loading here.  See MAX_CLIENT_TRANSLATORS
		//

		// since we only allocate one of each, don't bother pooling 'em
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") WindowTranslator,     10 );
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") MetaEventTranslator,	20 );
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") HotKeyTranslator,	25 );
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") PlaceEventTranslator,	30 );
		m_translators[ m_numTranslators++ ] = TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") GUICommandTranslator, 40 );
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") SelectionTranslator,	50 );
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") LookAtTranslator,			60 );
		m_translators[ m_numTranslators ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") CommandTranslator,		70 );
		// we keep a pointer to the command translator because it's useful
		m_commandTranslator = (CommandTranslator *)TheMessageStream->findTranslator( m_translators[ m_numTranslators++ ] );
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") HintSpyTranslator,		100 );

		//
		// the client message translator should probably remain as the last reaction of the
		// client before the messages are given to the network for processing.  This
		// lets all systems in the client give events that can be processed by the
		// client message translator
		//
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") GameClientMessageDispatcher, 999999999 );

	}

	// create the font library
	TheFontLibrary = createFontLibrary();
	if( TheFontLibrary )
		TheFontLibrary->init();

	// create the mouse
	TheMouse = TheGlobalData->m_headless ? NEW MouseDummy : createMouse();
	TheMouse->parseIni();
	TheMouse->initCursorResources();
 	TheMouse->setName("TheMouse");

	// instantiate the display
	TheDisplay = createGameDisplay();
	if( TheDisplay ) {
		try {
			TheDisplay->init();
		} catch (const std::exception& e) {
			fprintf(stderr, "ERROR: TheDisplay->init() threw exception: %s\n", e.what());
			throw;
		} catch (...) {
			fprintf(stderr, "ERROR: TheDisplay->init() threw unknown exception\n");
			throw;
		}
 		TheDisplay->setName("TheDisplay");
	}

	TheHeaderTemplateManager = MSGNEW("GameClientSubsystem") HeaderTemplateManager;
	if(TheHeaderTemplateManager){
		TheHeaderTemplateManager->init();
	}

	// create the window manager
	TheWindowManager = TheGlobalData->m_headless ? NEW GameWindowManagerDummy : createWindowManager();
	if( TheWindowManager )
	{

		TheWindowManager->init();
 		TheWindowManager->setName("TheWindowManager");
//		TheWindowManager->initTestGUI();

	}

	// create the IME manager
	TheIMEManager = CreateIMEManagerInterface();
	if ( TheIMEManager )
	{
		TheIMEManager->init();
 		TheIMEManager->setName("TheIMEManager");
	}

	// create the shell
	TheShell = MSGNEW("GameClientSubsystem") Shell;
	if( TheShell ) {
		TheShell->init();
 		TheShell->setName("TheShell");
	}

	// instantiate the in-game user interface
	TheInGameUI = createInGameUI();
	if( TheInGameUI ) {
		TheInGameUI->init();
 		TheInGameUI->setName("TheInGameUI");
	}

 	TheChallengeGenerals = createChallengeGenerals();
 	if( TheChallengeGenerals ) {
 		TheChallengeGenerals->init();
 	}

	TheHotKeyManager = MSGNEW("GameClientSubsystem") HotKeyManager;
	if( TheHotKeyManager ) {
		TheHotKeyManager->init();
 		TheHotKeyManager->setName("TheHotKeyManager");
	}

	// instantiate the terrain visual display
	TheTerrainVisual = createTerrainVisual();
	if( TheTerrainVisual ) {
		TheTerrainVisual->init();
 		TheTerrainVisual->setName("TheTerrainVisual");
	}

	// allocate the ray effects manager
	TheRayEffects = MSGNEW("GameClientSubsystem") RayEffectSystem;
	if( TheRayEffects )	{
		TheRayEffects->init();
 		TheRayEffects->setName("TheRayEffects");
	}

	// set the limits of the mouse now that we've created the display and such
	if( TheMouse )
	{
		// finish initializing the mouse.
		TheMouse->init();
		TheMouse->initCapture();
		TheMouse->setPosition( 0, 0 );
		TheMouse->setMouseLimits();
		TheMouse->setName("TheMouse");
	}

	// create the video player
	TheVideoPlayer = createVideoPlayer();
	if ( TheVideoPlayer )
	{
		TheVideoPlayer->init();
 		TheVideoPlayer->setName("TheVideoPlayer");
	}

	// create the language filter.
	TheLanguageFilter = createLanguageFilter();
	if (TheLanguageFilter)
	{
		TheLanguageFilter->init();
 		TheLanguageFilter->setName("TheLanguageFilter");
	}

	TheCampaignManager = MSGNEW("GameClientSubsystem") CampaignManager;
	TheCampaignManager->init();

	TheEva = MSGNEW("GameClientSubsystem") Eva;
	TheEva->init();
 	TheEva->setName("TheEva");

	TheDisplayStringManager->postProcessLoad();

	TheSnowManager = createSnowManager();
	if (TheSnowManager)
	{
		TheSnowManager->init();
		TheSnowManager->setName("TheSnowManager");
	}

#ifdef PERF_TIMERS
	TheGraphDraw = new GraphDraw;
#endif

}

//-------------------------------------------------------------------------------------------------
/** Reset the game client for a new game */
void GameClient::reset()
{
	Drawable *draw, *nextDraw;
//	m_drawableHash.clear();
//	m_drawableHash.resize(DRAWABLE_HASH_SIZE);

	m_drawableVector.clear();
	m_drawableVector.resize(DRAWABLE_HASH_SIZE, nullptr);

	// need to reset the in game UI to clear drawables before they are destroyed
	TheInGameUI->reset();

	// destroy all Drawables
	for( draw = m_drawableList; draw; draw = nextDraw )
	{
		nextDraw = draw->getNextDrawable();
		destroyDrawable( draw );
	}
	m_drawableList = nullptr;

	TheDisplay->reset();
	TheTerrainVisual->reset();
	TheRayEffects->reset();
	TheVideoPlayer->reset();
	TheEva->reset();
	if (TheSnowManager)
		TheSnowManager->reset();

	// clear any drawable TOC we might have
	m_drawableTOC.clear();

	// TheSuperHackers @fix Mauller 13/04/2025 Reset the drawable id so it does not keep growing over the lifetime of the game.
	m_nextDrawableID = (DrawableID)1;

}

/** -----------------------------------------------------------------------------------------------
 * Return a new unique object id.
 */
DrawableID GameClient::allocDrawableID()
{
	/// @todo Find unused value in current set
	DrawableID ret = m_nextDrawableID;
	m_nextDrawableID = (DrawableID)((UnsignedInt)m_nextDrawableID + 1);
	return ret;
}

/** -----------------------------------------------------------------------------------------------
 * Given a drawable, register it with the GameClient and give it a unique ID.
 */
void GameClient::registerDrawable( Drawable *draw )
{

	// assign this drawable a unique ID, this will add it to the fast lookup table too
	draw->setID( allocDrawableID() );

	// add the drawable to the master list
	draw->prependToList( &m_drawableList );

}

/** -----------------------------------------------------------------------------------------------
 * Redraw all views, update the GUI, play sound effects, etc.
 */
DECLARE_PERF_TIMER(GameClient_update)
DECLARE_PERF_TIMER(GameClient_draw)
void GameClient::update()
{
	USE_PERF_TIMER(GameClient_update)
	PROFILER_FRAME_MARK;
	PROFILER_SECTION_COLOR(0x2196F3);
	// Counts this frame and emits once a second, from a destructor so that the intro early return
	// below is counted too. frame.client counts every call to this function; if the phases counted
	// only some of them the two would not be comparable.
	GX_FRAME_END();
	// Everything from here to the intro check is input and 2D housekeeping: snow, anim2d,
	// keyboard, Eva, mouse. Marked rather than scoped because it is not a braced block.
	const double gxInputT0 = GX_NOW();
	// create the FRAME_TICK message
	GameMessage *frameMsg = TheMessageStream->appendMessage( GameMessage::MSG_FRAME_TICK );
	frameMsg->appendTimestampArgument( getFrame() );
	static Bool playSizzle = FALSE;
	// We need to show the movie first.
	if(TheGlobalData->m_playIntro && !TheDisplay->isMoviePlaying())
	{
		if(TheGameLODManager && TheGameLODManager->didMemPass())
			TheDisplay->playLogoMovie("EALogoMovie", 5000, 3000);
		else
			TheDisplay->playLogoMovie("EALogoMovie640", 5000, 3000);
		TheWritableGlobalData->m_playIntro = FALSE;
		TheWritableGlobalData->m_afterIntro = TRUE;
		playSizzle = TRUE;
	}

	//Initial Game Condition.  We must show the movie first and then we can display the shell
	if(TheGlobalData->m_afterIntro && !TheDisplay->isMoviePlaying())
	{
		if( playSizzle && TheGlobalData->m_playSizzle )
		{
			TheWritableGlobalData->m_allowExitOutOfMovies = TRUE;
			if(TheGameLODManager && TheGameLODManager->didMemPass())
				TheDisplay->playMovie("Sizzle");
			else
				TheDisplay->playMovie("Sizzle640");
			playSizzle = FALSE;
		}
		else
		{
			TheWritableGlobalData->m_breakTheMovie = TRUE;
			TheWritableGlobalData->m_allowExitOutOfMovies = TRUE;

			if(TheGameLODManager && !TheGameLODManager->didMemPass())
			{
				TheWritableGlobalData->m_breakTheMovie = FALSE;

				WindowLayout *legal = TheWindowManager->winCreateLayout("Menus/LegalPage.wnd");
				if(legal)
				{
					legal->hide(FALSE);
					legal->bringForward();
					Int beginTime = timeGetTime();
					while(beginTime + 4000 > timeGetTime() )
					{
						if (GameClient::isMovieAbortRequested())
						{
							break;
						}

						TheWindowManager->update();
						// redraw all views, update the GUI
						TheDisplay->draw();
						Sleep(100);
					}
					setFPMode();


					legal->destroyWindows();
					deleteInstance(legal);

				}
				TheWritableGlobalData->m_breakTheMovie = TRUE;


			}

		TheShell->showShellMap(TRUE);
		TheShell->showShell();
		TheWritableGlobalData->m_afterIntro = FALSE;
		}
	}

	//Update snow particles.
	if (TheSnowManager)
		TheSnowManager->UPDATE();

	// update animation 2d collection
	TheAnim2DCollection->UPDATE();

	// update the keyboard
	if( TheKeyboard )
	{
		TheKeyboard->UPDATE();
		TheKeyboard->createStreamMessages();

	}

	// Update the Eva stuff
	TheEva->UPDATE();

	// update the mouse
	if( TheMouse )
	{
		TheMouse->UPDATE();
		TheMouse->createStreamMessages();

	}


  if (TheInGameUI->isCameraTrackingDrawable())
  {
    Drawable *draw = TheInGameUI->getFirstSelectedDrawable();
    if ( draw )
    {
      TheTacticalView->userLookAt( draw->getPosition() );
    }
    else
      TheInGameUI->setCameraTrackingDrawable( FALSE );
  }

	GX_ADD(g_cInput, gxInputT0);

	if(TheGlobalData->m_playIntro || TheGlobalData->m_afterIntro)
	{
		// redraw all views, update the GUI. Timed into the SAME phases as the main path: these
		// frames are cheaper, and leaving them out of the numerator while frame.client kept them
		// in the denominator is precisely what made the residual go negative.
		{ GX_PHASE(g_cDispUpd);  TheDisplay->UPDATE(); }
		{ GX_PHASE(g_cDispDraw); TheDisplay->DRAW(); }
		return;   // GX_FRAME_END still counts this frame — it runs from a destructor
	}

	// update the window system itself
	{
		GX_PHASE(g_cWindow);
		TheWindowManager->UPDATE();
	}

	// update the video player
	{
		GX_PHASE(g_cVideo);
		TheVideoPlayer->UPDATE();
	}

	const Bool freezeTime = TheGameEngine->isTimeFrozen() || TheGameEngine->isGameHalted();

	const Int localPlayerIndex = rts::getObservedOrLocalPlayer()->getPlayerIndex();

	// Ghost-object bookkeeping, per-drawable shroud classification, and updateDrawable() for every
	// drawable in the world. Timed as one phase: they are one loop over the same set, and splitting
	// them would add two clock reads per drawable rather than two per frame.
	const double gxDrawablesT0 = GX_NOW();
	if (!freezeTime)
	{
		Int numPlayers = ThePlayerList->getPlayerCount();
		Int numNonLocalPlayers = 0;
		Int nonLocalPlayerIndices[MAX_PLAYER_COUNT];

#if ENABLE_CONFIGURABLE_SHROUD
		if (TheGlobalData->m_shroudOn)
#else
		if (true)
#endif
		{
			if (TheGhostObjectManager->trackAllPlayers())
			{
				//Find indices of all active players
				for (Int i=0; i < numPlayers; i++)
				{
					Player *player = ThePlayerList->getNthPlayer(i);
					if (player->getPlayerTemplate() != nullptr && player->getPlayerIndex() != localPlayerIndex)
						nonLocalPlayerIndices[numNonLocalPlayers++] = player->getPlayerIndex();
				}
				//update ghost objects which don't have drawables or objects.
				TheGhostObjectManager->updateOrphanedObjects(nonLocalPlayerIndices, numNonLocalPlayers);
			}
			else
			{
				TheGhostObjectManager->updateOrphanedObjects(nullptr, 0);
			}
		}


		// call the update for all client drawables
		Drawable* draw = firstDrawable();
		while (draw)
		{	// update() could free the Drawable, so go ahead and grab 'next'
			Drawable* next = draw->getNextDrawable();
#if ENABLE_CONFIGURABLE_SHROUD
			if (TheGlobalData->m_shroudOn)
#else
			if (true)
#endif
			{
				//immobile objects need to take snapshots whenever they become fogged
				//so need to refresh their status.  We can't rely on external calls
				//to getShroudStatus() because they are only made for visible on-screen
				//objects.
				Object *object=draw->getObject();
				if (object)
				{
					if (TheGhostObjectManager->trackAllPlayers())
					{
						// TheSuperHackers @info Update the shrouded status for all objects
						// that own a ghost object for all non local players. This is costly.
						if (object->hasGhostObject())
						{
							Int *playerIndex = nonLocalPlayerIndices;
							Int *const playerIndexEnd = nonLocalPlayerIndices + numNonLocalPlayers;
							for (; playerIndex < playerIndexEnd; ++playerIndex)
							{
								object->getShroudedStatus(*playerIndex);
							}
						}
					}

					ObjectShroudStatus ss=object->getShroudedStatus(localPlayerIndex);
					if (ss >= OBJECTSHROUD_FOGGED && draw->getShroudClearFrame() != InvalidShroudClearFrame) {
						UnsignedInt limit = 2*LOGICFRAMES_PER_SECOND;
						if (object->isEffectivelyDead()) {
							// extend the time, so we can see the dead plane blow up & crash.
							limit += 3*LOGICFRAMES_PER_SECOND;
						}
						if (TheGameLogic->getFrame() < limit + draw->getShroudClearFrame()) {
							// It's been less than 2 seconds since we could see them clear, so keep showing them.
							ss = OBJECTSHROUD_CLEAR;
						}
					}
					draw->setFullyObscuredByShroud(ss >= OBJECTSHROUD_FOGGED);
				}
			}
			draw->updateDrawable();
			draw = next;
		}
	}

#if defined(RTS_DEBUG)
	// need to draw the first frame, then don't draw again until TheGlobalData->m_noDraw
	if (TheGlobalData->m_noDraw > TheGameLogic->getFrame() && TheGameLogic->getFrame() > 0)
	{
		return;
	}
#endif

	// update all particle systems
	if( !freezeTime )
	{
		// update particle systems
		TheParticleSystemManager->setLocalPlayerIndex(localPlayerIndex);
//		TheParticleSystemManager->update();

	}

	GX_ADD(g_cDrawables, gxDrawablesT0);

	// update the terrain visuals
	{
		GX_PHASE(g_cTerrain);
		TheTerrainVisual->UPDATE();
	}

	// update display
	{
		GX_PHASE(g_cDispUpd);
		TheDisplay->UPDATE();
	}

	{
		USE_PERF_TIMER(GameClient_draw)
		// The 3D render. Every GL call the SDK times lives inside here, so
		// client.display_draw_ms MINUS the gl.* subtotal is the scene work that produces those
		// calls -- traversal, culling, render-list building -- which no gauge has ever separated
		// from the driver cost it feeds.
		GX_PHASE(g_cDispDraw);

	// redraw all views, update the GUI
	//if(TheGameLogic->getFrame() >= 2)

		TheDisplay->DRAW();
	}

	{
		// let display string factory handle its update
		GX_PHASE(g_cStrings);
		TheDisplayStringManager->update();
	}

	{
		// update the shell
		GX_PHASE(g_cShell);
		TheShell->UPDATE();
	}

	{
		// update the in game UI
		GX_PHASE(g_cUI);
		TheInGameUI->UPDATE();
	}

}

#ifdef __EMSCRIPTEN__
namespace {
// Runs on EVERY exit from GameClient::update(), including the intro early return -- see the type's
// declaration for why that matters.
GxFrameEnd::~GxFrameEnd()
{
	g_cAccFrames += 1.0;
	const double now = emscripten_get_now();
	if (g_cLastEmit == 0.0) g_cLastEmit = now;
	if (now - g_cLastEmit < 1000.0 || g_cAccFrames <= 0.0) return;

	const double n = g_cAccFrames;
	// The denominator itself, because an average whose divisor is invisible cannot be checked
	// against anything. It also reports the real frame rate directly: frame.client is only PART of
	// the wall-clock frame, so 1000/frame.client is not fps and reading it as fps overstated this
	// port by 3x once.
	dx8wasm_tel_gauge("client.frames",            n);
	dx8wasm_tel_gauge("client.input_ms",          g_cInput     / n);
	dx8wasm_tel_gauge("client.window_ms",         g_cWindow    / n);
	dx8wasm_tel_gauge("client.video_ms",          g_cVideo     / n);
	dx8wasm_tel_gauge("client.drawables_ms",      g_cDrawables / n);
	dx8wasm_tel_gauge("client.terrain_ms",        g_cTerrain   / n);
	dx8wasm_tel_gauge("client.display_update_ms", g_cDispUpd   / n);
	dx8wasm_tel_gauge("client.display_draw_ms",   g_cDispDraw  / n);
	dx8wasm_tel_gauge("client.strings_ms",        g_cStrings   / n);
	dx8wasm_tel_gauge("client.shell_ms",          g_cShell     / n);
	dx8wasm_tel_gauge("client.ingameui_ms",       g_cUI        / n);
	// The completeness check. frame.client - client.accounted_ms is what is STILL un-timed inside
	// update(); if that residual is large the instrument is incomplete and says so, rather than
	// letting a reader assume these phases are the whole frame. It is deliberately SIGNED: a
	// negative residual is impossible in a correct accounting, and it was a negative residual that
	// exposed the denominator mismatch this destructor exists to fix.
	dx8wasm_tel_gauge("client.accounted_ms",
		(g_cInput + g_cWindow + g_cVideo + g_cDrawables + g_cTerrain +
		 g_cDispUpd + g_cDispDraw + g_cStrings + g_cShell + g_cUI) / n);
	// Input-loss diagnosis (OPEN-ITEMS §0o): the GUI/engine state that decides whether a click can do
	// anything, sampled with the frame gauges so a capture of "the mouse does nothing" carries it.
	//   gui.modal: 0 none, 1 a live root window, 2 an entry whose window is no longer a root window
	//   (destroyed while NOT at the top of the modal stack: winDestroy/processDestroyList only check
	//   the head, so such an entry is never popped and every mouse event would route into it).
	if (TheWindowManager) {
		GameWindow *modal = TheWindowManager->winGetModalWindow();
		dx8wasm_tel_gauge("gui.modal", modal ? (TheWindowManager->winIsRootWindow(modal) ? 1.0 : 2.0) : 0.0);
		dx8wasm_tel_gauge("gui.captor", TheWindowManager->winGetCapture() ? 1.0 : 0.0);
		dx8wasm_tel_gauge("gui.grab", TheWindowManager->winGetGrabWindow() ? 1.0 : 0.0);
		dx8wasm_tel_gauge("gui.kbd_focus", TheWindowManager->winGetFocus() ? 1.0 : 0.0);
	}
	if (TheShell) {
		WindowLayout *opt = TheShell->getOptionsLayout(FALSE);
		dx8wasm_tel_gauge("gui.options_open", (opt && !opt->isHidden()) ? 1.0 : 0.0);
	}
	if (TheInGameUI) {
		dx8wasm_tel_gauge("gui.quit_menu_visible", TheInGameUI->isQuitMenuVisible() ? 1.0 : 0.0);   // SelectionTranslator drops every left click while this is set
		dx8wasm_tel_gauge("gui.input_enabled", TheInGameUI->getInputEnabled() ? 1.0 : 0.0);
	}
	if (TheGameLogic) dx8wasm_tel_gauge("logic.paused", TheGameLogic->isGamePaused() ? 1.0 : 0.0);
	if (TheGameEngine) dx8wasm_tel_gauge("engine.active", TheGameEngine->isActive() ? 1.0 : 0.0);   // SDL window focus; canOpenQuitMenu() needs it
	g_cAccFrames = 0.0;
	g_cInput = g_cWindow = g_cVideo = g_cDrawables = g_cTerrain = 0.0;
	g_cDispUpd = g_cDispDraw = g_cStrings = g_cShell = g_cUI = 0.0;
	g_cLastEmit = now;
}
}  // namespace
#endif

void GameClient::step()
{
	TheDisplay->step();
}

void GameClient::updateHeadless()
{
	// TheSuperHackers @info helmutbuhler 03/05/2025 bobtista 02/02/2026
	// Update particles to prevent accumulation in headless mode. Particles are generated
	// during GameLogic and only cleaned up during rendering. update() lets particles finish
	// their lifecycle naturally instead of abruptly removing them with reset().
	TheParticleSystemManager->update();
}

Bool GameClient::isMovieAbortRequested()
{
	if (TheGameEngine)
	{
		TheGameEngine->serviceWindowsOS();
	}

	// TheSuperHackers @feature User can skip video by pressing ESC
	if (TheKeyboard)
	{
		TheKeyboard->UPDATE();
		KeyboardIO *io = TheKeyboard->findKey(KEY_ESC, KeyboardIO::STATUS_UNUSED);
		if (io && BitIsSet(io->state, KEY_STATE_DOWN))
		{
			io->setUsed();
			return TRUE;
		}
	}

	if (TheGameEngine && TheGameEngine->getQuitting())
	{
		return TRUE;
	}

	if (TheMessageStream && TheMessageStream->containsMessageOfType(GameMessage::MSG_META_DEMO_INSTANT_QUIT))
	{
		return TRUE;
	}

	if (TheGameLogic && TheGameLogic->isQuitToDesktopRequested())
	{
		return TRUE;
	}

	return FALSE;
}

/** -----------------------------------------------------------------------------------------------
 * Call the given callback function for each object contained within the given region.
 */
void GameClient::iterateDrawablesInRegion( Region3D *region, GameClientFuncPtr userFunc, void *userData )
{
	Drawable *draw, *nextDrawable;

	for( draw = m_drawableList; draw; draw=nextDrawable )
	{
		nextDrawable = draw->getNextDrawable();

		Coord3D pos = *draw->getPosition();
		if( region == nullptr ||
			  (pos.x >= region->lo.x && pos.x <= region->hi.x &&
			   pos.y >= region->lo.y && pos.y <= region->hi.y &&
				 pos.z >= region->lo.z && pos.z <= region->hi.z) )
		{
			(*userFunc)( draw, userData );
		}
	}
}

/**Helper function to update fake GLA structures to become visible to certain players.
We should only call this during critical moments, such as changing teams, changing to
observer, etc.*/
void GameClient::updateFakeDrawables()
{
	for( Drawable *draw = getDrawableList(); draw; draw = draw->getNextDrawable() )
	{
		const Object *object=draw->getObject();

		if( object && object->isKindOf( KINDOF_FS_FAKE ) )
		{
			Relationship rel = rts::getObservedOrLocalPlayer()->getRelationship(object->getTeam());
			if (rel == ALLIES || rel == NEUTRAL)
				draw->setTerrainDecal(TERRAIN_DECAL_SHADOW_TEXTURE);
			else
				draw->setTerrainDecal(TERRAIN_DECAL_NONE);
		}
	}
}

/** -----------------------------------------------------------------------------------------------
 * Destroy the drawable immediately.
 */
void GameClient::destroyDrawable( Drawable *draw )
{

	// remove any notion of the Drawable in the in-game user interface
	TheInGameUI->disregardDrawable( draw );

	// remove from the master list
	draw->removeFromList(&m_drawableList);

	//
	// because drawables and objects are tightly coupled, not only MUST we maintain
	// our links in all instances, but it is NECESSARY for the client to actually
	// modify data in the logic, that is the pointer in an object to *this* drawable
	//
	Object *obj = draw->getObject();
	if( obj )
	{

		DEBUG_ASSERTCRASH( obj->getDrawable() == draw, ("Object/Drawable pointer mismatch!") );
		obj->friend_bindToDrawable( nullptr );

	}

	// remove the drawable from our hash of drawables
	removeDrawableFromLookupTable( draw );

	// free storage
	deleteInstance(draw);

}

// ------------------------------------------------------------------------------------------------
/** Add drawable to lookup table for fast id searching */
// ------------------------------------------------------------------------------------------------
void GameClient::addDrawableToLookupTable(Drawable *draw )
{

	// sanity
	if( draw == nullptr )
		return;

	// add to lookup
//	m_drawableHash[ draw->getID() ] = draw;
	DrawableID newID = draw->getID();
	while( newID >= m_drawableVector.size() ) // Fail case is hella rare, so faster to double up on size() call
		m_drawableVector.resize(m_drawableVector.size() * 2, nullptr);

	m_drawableVector[ newID ] = draw;

}

// ------------------------------------------------------------------------------------------------
/** Remove drawable from lookup table of fast id searching */
// ------------------------------------------------------------------------------------------------
void GameClient::removeDrawableFromLookupTable( Drawable *draw )
{

	// sanity
	// TheSuperHackers @fix Mauller/Xezon 24/04/2025 Prevent out of range access to vector lookup table
	if( draw == nullptr || static_cast<size_t>(draw->getID()) >= m_drawableVector.size() )
		return;

	// remove from table
//	m_drawableHash.erase( draw->getID() );
	m_drawableVector[ draw->getID() ] = nullptr;

}

//-------------------------------------------------------------------------------------------------
/** Load a map into the game interface */
Bool GameClient::loadMap( AsciiString mapName )
{

	// sanity
	if( mapName.isEmpty() )
		return false;

	assert( 0 );  // who calls this?

	return TRUE;

}

//-------------------------------------------------------------------------------------------------
/** Unload a map from the game interface */
void GameClient::unloadMap( AsciiString mapName )
{

	assert( 0 );  // who calls this?

}

//-------------------------------------------------------------------------------------------------
void GameClient::setTimeOfDay( TimeOfDay tod )
{
	Drawable *draw = firstDrawable();

	while( draw )
	{
		draw->setTimeOfDay( tod );

		draw = draw->getNextDrawable();
	}
}

//-------------------------------------------------------------------------------------------------
void GameClient::assignSelectedDrawablesToGroup( Int group )
{
/*
	Drawable *draw = firstDrawable();
	while( draw )
	{

		if( draw->isSelected() && draw->getObject()->isLocallyControlled())
		{
			draw->setDrawableGroup( group );
		}
		else if( draw->getDrawableGroup() == group )
		{
			draw->setDrawableGroup( 0 );
		}

		draw = draw->getNextDrawable();

	}
*/
}

//-------------------------------------------------------------------------------------------------
void GameClient::selectDrawablesInGroup( Int group )
{
/*
	Drawable *draw = firstDrawable();

	// create a message that will assign a group ID to all the selected drawables, this
	// way when we do things with this current selected group of units, we only have
	// to refer to the group ID and not each individual object ID
	GameMessage *teamMsg = TheMessageStream->appendMessage( GameMessage::MSG_CREATE_SELECTED_GROUP );

	//We are creating a new group from scratch.
	teamMsg->appendBooleanArgument( true );

	while( draw )
	{
		int counter = 0;

		const Object *object = draw->getObject();

		if( object && draw->getDrawableGroup() == group && object->isLocallyControlled() && !object->isContained() )
		{
			//Only select the object if it is locally controlled and not contained by anything.
			TheInGameUI->selectDrawable(draw);
			teamMsg->appendObjectIDArgument( draw->getObject()->getID() );
		}
		else
		{
			TheInGameUI->deselectDrawable(draw);
		}

		draw = draw->getNextDrawable();
		counter++;
	}

*/
}

// ------------------------------------------------------------------------------------------------
void GameClient::addTextBearingDrawable( Drawable *tbd )
{
	if ( tbd != nullptr )
		m_textBearingDrawableList.push_back( tbd );
}
// ------------------------------------------------------------------------------------------------
void GameClient::flushTextBearingDrawables()
{

	/////////////////////////////
	// WALK THIS LIST AND CALL EACH DRAWABLES TEXTY STUFF
	/////////////////////////////

	for( TextBearingDrawableListIterator it = m_textBearingDrawableList.begin(); it != m_textBearingDrawableList.end(); ++it )
	  (*it)->drawUIText();

	m_textBearingDrawableList.clear();
}

// ------------------------------------------------------------------------------------------------
GameMessage::Type GameClient::evaluateContextCommand( Drawable *draw,
																											const Coord3D *pos,
																											CommandTranslator::CommandEvaluateType cmdType )
{

	if( m_commandTranslator )
		return m_commandTranslator->evaluateContextCommand( draw, pos, cmdType );
	else
		return GameMessage::MSG_INVALID;

}

//-------------------------------------------------------------------------------------------------
/** Get the ray effect data for a drawable */
void GameClient::getRayEffectData( Drawable *draw, RayEffectData *effectData )
{

	TheRayEffects->getRayEffectData( draw, effectData );

}

//-------------------------------------------------------------------------------------------------
/** remove the drawable from the ray effects system if present */
void GameClient::removeFromRayEffects( Drawable *draw )
{

	TheRayEffects->deleteRayEffect( draw );

}

/** frees all shadow resources used by this module - used by Options screen.*/
void GameClient::releaseShadows()
{
	Drawable *draw;
	for( draw = firstDrawable(); draw; draw = draw->getNextDrawable() )
		draw->releaseShadows();
}

/** create shadow resources if not already present. Used by Options screen.*/
void GameClient::allocateShadows()
{
	Drawable *draw;
	for( draw = firstDrawable(); draw; draw = draw->getNextDrawable() )
		draw->allocateShadows();
}

//-------------------------------------------------------------------------------------------------
/** Preload assets for the currently loaded map.  Those assets include all the damage states
	* for every building loaded, as well as any faction units/structures we can build and
	* all their damage states */
//-------------------------------------------------------------------------------------------------
void GameClient::preloadAssets( TimeOfDay timeOfDay )
{

	MEMORYSTATUS before, after;
	GlobalMemoryStatus(&before);

	// first, for every drawable in the map load the assets for all states we care about
	Drawable *draw;
	for( draw = firstDrawable(); draw; draw = draw->getNextDrawable() )
		draw->preloadAssets( timeOfDay );

	//
	// now create a temporary drawable for each of the faction things we can create, preload
	// their assets, and dump the drawable
	//
	AsciiString side;
	const ThingTemplate *tTemplate;
	for( tTemplate = TheThingFactory->firstTemplate();
			 tTemplate;
			 tTemplate = tTemplate->friend_getNextTemplate() )
	{

		// if this isn't one of the objects that can be preloaded ignore it
		if( tTemplate->isKindOf( KINDOF_PRELOAD ) == FALSE && !TheGlobalData->m_preloadEverything )
			continue;

		// create the drawable and do the preloading
		draw = TheThingFactory->newDrawable( tTemplate );
		if( draw )
		{

			// preload the assets
			draw->preloadAssets( timeOfDay );

			// destroy the drawable
			destroyDrawable( draw );

		}

	}
	GlobalMemoryStatus(&after);

	DEBUG_LOG(("Preloading memory dwAvailPageFile %d --> %d : %d",
		before.dwAvailPageFile, after.dwAvailPageFile, before.dwAvailPageFile - after.dwAvailPageFile));
	DEBUG_LOG(("Preloading memory dwAvailPhys     %d --> %d : %d",
		before.dwAvailPhys, after.dwAvailPhys, before.dwAvailPhys - after.dwAvailPhys));
	DEBUG_LOG(("Preloading memory dwAvailVirtual  %d --> %d : %d",
		before.dwAvailVirtual, after.dwAvailVirtual, before.dwAvailVirtual - after.dwAvailVirtual));
	/*
	DEBUG_LOG(("Preloading memory dwLength        %d --> %d : %d",
		before.dwLength, after.dwLength, before.dwLength - after.dwLength));
	DEBUG_LOG(("Preloading memory dwMemoryLoad    %d --> %d : %d",
		before.dwMemoryLoad, after.dwMemoryLoad, before.dwMemoryLoad - after.dwMemoryLoad));
	DEBUG_LOG(("Preloading memory dwTotalPageFile %d --> %d : %d",
		before.dwTotalPageFile, after.dwTotalPageFile, before.dwTotalPageFile - after.dwTotalPageFile));
	DEBUG_LOG(("Preloading memory dwTotalPhys     %d --> %d : %d",
		before.dwTotalPhys , after.dwTotalPhys, before.dwTotalPhys - after.dwTotalPhys));
	DEBUG_LOG(("Preloading memory dwTotalVirtual  %d --> %d : %d",
		before.dwTotalVirtual , after.dwTotalVirtual, before.dwTotalVirtual - after.dwTotalVirtual));
	*/

	GlobalMemoryStatus(&before);
	extern std::vector<AsciiString>	debrisModelNamesGlobalHack;
	size_t i=0;
	for (; i<debrisModelNamesGlobalHack.size(); ++i)
	{
		TheDisplay->preloadModelAssets(debrisModelNamesGlobalHack[i]);
	}
	GlobalMemoryStatus(&after);
	debrisModelNamesGlobalHack.clear();

	DEBUG_LOG(("Preloading memory dwAvailPageFile %d --> %d : %d",
		before.dwAvailPageFile, after.dwAvailPageFile, before.dwAvailPageFile - after.dwAvailPageFile));
	DEBUG_LOG(("Preloading memory dwAvailPhys     %d --> %d : %d",
		before.dwAvailPhys, after.dwAvailPhys, before.dwAvailPhys - after.dwAvailPhys));
	DEBUG_LOG(("Preloading memory dwAvailVirtual  %d --> %d : %d",
		before.dwAvailVirtual, after.dwAvailVirtual, before.dwAvailVirtual - after.dwAvailVirtual));

	TheControlBar->preloadAssets( timeOfDay );

	GlobalMemoryStatus(&before);
	TheParticleSystemManager->preloadAssets( timeOfDay );
	GlobalMemoryStatus(&after);

	DEBUG_LOG(("Preloading memory dwAvailPageFile %d --> %d : %d",
		before.dwAvailPageFile, after.dwAvailPageFile, before.dwAvailPageFile - after.dwAvailPageFile));
	DEBUG_LOG(("Preloading memory dwAvailPhys     %d --> %d : %d",
		before.dwAvailPhys, after.dwAvailPhys, before.dwAvailPhys - after.dwAvailPhys));
	DEBUG_LOG(("Preloading memory dwAvailVirtual  %d --> %d : %d",
		before.dwAvailVirtual, after.dwAvailVirtual, before.dwAvailVirtual - after.dwAvailVirtual));

	const char *const textureNames[] = {
		"ptspruce01.tga",
		"exrktflame.tga",
		"cvlimo3_d2.tga",
		"exfthrowerstream.tga",
		"uvrockbug_d1.tga",
		"arcbackgroundc.tga",
		"grade3.tga",
		"framebasec.tga",
		"gradec.tga",
		"frametopc.tga",
		"arcbackgrounda.tga",
		"arcglow2.tga",
		"framebasea.tga",
		"gradea.tga",
		"frametopa.tga",
		"sauserinterface256_002.tga",
		"sauserinterface256_001.tga",
		"unitbackgrounda.tga",
		"sauserinterface256_004.tga",
		"sagentank.tga",
		"sauserinterface256_005.tga",
		"sagenair.tga",
		"sauserinterface256_003.tga",
		"sagenspec.tga",
		"snuserinterface256_003.tga",
		"snuserinterface256_002.tga",
		"unitbackgroundc.tga",
		"snuserinterface256_004.tga",
		"sngenredarm.tga",
		"snuserinterface256_001.tga",
		"sngenspewea.tga",
		"sngensecpol.tga",
		"ciburn.tga",
		"ptmaple02.tga",
		"scuserinterface256_005.tga",
		"scuserinterface256_002.tga",
		"sauserinterface256_006.tga",
		"pmcrates.tga",
		""
	};

	GlobalMemoryStatus(&before);
	for (i=0; *textureNames[i]; ++i)
		TheDisplay->preloadTextureAssets(textureNames[i]);
	GlobalMemoryStatus(&after);

	DEBUG_LOG(("Preloading memory dwAvailPageFile %d --> %d : %d",
		before.dwAvailPageFile, after.dwAvailPageFile, before.dwAvailPageFile - after.dwAvailPageFile));
	DEBUG_LOG(("Preloading memory dwAvailPhys     %d --> %d : %d",
		before.dwAvailPhys, after.dwAvailPhys, before.dwAvailPhys - after.dwAvailPhys));
	DEBUG_LOG(("Preloading memory dwAvailVirtual  %d --> %d : %d",
		before.dwAvailVirtual, after.dwAvailVirtual, before.dwAvailVirtual - after.dwAvailVirtual));

//	preloadTextureNamesGlobalHack2 = preloadTextureNamesGlobalHack;
//	preloadTextureNamesGlobalHack.clear();

}

// ------------------------------------------------------------------------------------------------
/** Given a string name, find the drawable TOC entry (if any) associated with it */
// ------------------------------------------------------------------------------------------------
GameClient::DrawableTOCEntry *GameClient::findTOCEntryByName( AsciiString name )
{

	for( DrawableTOCListIterator it = m_drawableTOC.begin(); it != m_drawableTOC.end(); ++it )
		if( (*it).name == name )
			return &(*it);

	return nullptr;

}

// ------------------------------------------------------------------------------------------------
/** Given a drawable TOC identifier, find the drawable TOC if any */
// ------------------------------------------------------------------------------------------------
GameClient::DrawableTOCEntry *GameClient::findTOCEntryById( UnsignedShort id )
{

	for( DrawableTOCListIterator it = m_drawableTOC.begin(); it != m_drawableTOC.end(); ++it )
		if( (*it).id == id )
			return &(*it);

	return nullptr;

}

// ------------------------------------------------------------------------------------------------
/** Add an drawable TOC entry */
// ------------------------------------------------------------------------------------------------
void GameClient::addTOCEntry( AsciiString name, UnsignedShort id )
{

	DrawableTOCEntry tocEntry;
	tocEntry.name = name;
	tocEntry.id = id;
	m_drawableTOC.push_back( tocEntry );

}

// ------------------------------------------------------------------------------------------------
static Bool shouldSaveDrawable(const Drawable* draw)
{
	if (draw->testDrawableStatus(DRAWABLE_STATUS_NO_SAVE))
	{
		if (draw->getObject() == nullptr)
		{
			return false;
		}
		else
		{
			DEBUG_CRASH(("You should not ever set DRAWABLE_STATUS_NO_SAVE for a Drawable with an object. (%s)",draw->getTemplate()->getName().str()));
		}
	}
	return true;
}

// ------------------------------------------------------------------------------------------------
/** Xfer drawable table of contents */
// ------------------------------------------------------------------------------------------------
void GameClient::xferDrawableTOC( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// clear our current table of contents
	m_drawableTOC.clear();

	// xfer the table
	UnsignedInt tocCount = 0;
	if( xfer->getXferMode() == XFER_SAVE )
	{
		AsciiString templateName;

		// generate a new TOC based on the drawables that are in the map
		for( Drawable *draw = getDrawableList(); draw; draw = draw->getNextDrawable() )
		{
			if (!shouldSaveDrawable(draw))
				continue;

			// get the name we're working with
			templateName = draw->getTemplate()->getName();

			// if is this drawable name already in the TOC, skip it
			if( findTOCEntryByName( templateName ) != nullptr )
				continue;

			// add this entry to the TOC
			addTOCEntry( draw->getTemplate()->getName(), ++tocCount );

		}

		// xfer entries in the TOC
		xfer->xferUnsignedInt( &tocCount );

		// xfer each TOC entry
		DrawableTOCListIterator it;
		DrawableTOCEntry *tocEntry;
		for( it = m_drawableTOC.begin(); it != m_drawableTOC.end(); ++it )
		{

			// get this toc entry
			tocEntry = &(*it);

			// xfer the name
			xfer->xferAsciiString( &tocEntry->name );

			// xfer the paired id
			xfer->xferUnsignedShort( &tocEntry->id );

		}

	}
	else
	{
		AsciiString templateName;
		UnsignedShort id;

		// how many entries are we going to read
		xfer->xferUnsignedInt( &tocCount );

		// read all the entries
		for( UnsignedInt i = 0; i < tocCount; ++i )
		{

			// read the name
			xfer->xferAsciiString( &templateName );

			// read the id
			xfer->xferUnsignedShort( &id );

			// add this to the TOC
			addTOCEntry( templateName, id );

		}

	}

}

// ------------------------------------------------------------------------------------------------
/** Xfer method for Game Client
  * Version History:
  * 1: Initial
  * 2: Adding mission briefing history
	* 3: Added block markers around drawable data, no version checking is done and therefore
	*		 this version breaks compatibility with previous versions. (CBD)
 */
// ------------------------------------------------------------------------------------------------
void GameClient::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 3;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// client frame number
	xfer->xferUnsignedInt( &m_frame );

	//
	// note that we do not do the id counter here, we did it in the game state block because
	// it's important to do that part very early in the load process
	//
	// !!!DON'T DO THIS!!! ----> xfer->xferDrawableID( &m_nextDrawableID ); <---- !!!DON'T DO THIS!!!

	//
	// xfer a table of contents that contain thing template and identifier pairs.  this
	// table of contents is good for this save file only as unique numbers are only
	// generated and stored for the actual things that are on this map
	//
	xferDrawableTOC( xfer );

	// drawable count
	Drawable *draw;
	UnsignedShort drawableCount = 0;
	for( draw = getDrawableList(); draw; draw = draw->getNextDrawable() )
	{
		if (xfer->getXferMode() == XFER_SAVE && !shouldSaveDrawable(draw))
			continue;
		drawableCount++;
	}
	xfer->xferUnsignedShort( &drawableCount );

	// drawable data
	DrawableTOCEntry *tocEntry;
	ObjectID objectID;
	if( xfer->getXferMode() == XFER_SAVE )
	{

		// iterate all drawables
		for( draw = getDrawableList(); draw; draw = draw->getNextDrawable() )
		{
			if (!shouldSaveDrawable(draw))
				continue;

			// get TOC entry for this drawable
			tocEntry = findTOCEntryByName( draw->getTemplate()->getName() );
			if( tocEntry == nullptr )
			{

				DEBUG_CRASH(( "GameClient::xfer - Drawable TOC entry not found for '%s'", draw->getTemplate()->getName().str() ));
				throw SC_INVALID_DATA;

			}

			// xfer toc id entry
			xfer->xferUnsignedShort( &tocEntry->id );

			// begin data block
			xfer->beginBlock();

			// write the object ID this drawable is attached to
			objectID = draw->getObject() ? draw->getObject()->getID() : INVALID_ID;
			xfer->xferObjectID( &objectID );

			// write snapshot data
			xfer->xferSnapshot( draw );

			// end data block
			xfer->endBlock();

		}

	}
	else
	{
		UnsignedShort tocID;
		const ThingTemplate *thingTemplate;
		Int dataSize;

		// read all entries
		for( UnsignedShort i = 0; i < drawableCount; ++i )
		{

			// read toc id entry
			xfer->xferUnsignedShort( &tocID );

			// find TOC entry with this identifier
			tocEntry = findTOCEntryById( tocID );
			if( tocEntry == nullptr )
			{

				DEBUG_CRASH(( "GameClient::xfer - No TOC entry match for id '%d'", tocID ));
				throw SC_INVALID_DATA;

			}

			// read data block size
			dataSize = xfer->beginBlock();

			// find matching thing template
			thingTemplate = TheThingFactory->findTemplate( tocEntry->name );
			if( thingTemplate == nullptr )
			{

				DEBUG_CRASH(( "GameClient::xfer - Unrecognized thing template '%s', skipping.  ENGINEERS - Are you *sure* it's OK to be ignoring this object from the save file???  Think hard about it!",
											tocEntry->name.str() ));
				xfer->skip( dataSize );
				continue;

			}

			// read the object ID this drawable is attached to (if any)
			xfer->xferObjectID( &objectID );

			//
			// if we have an attached object ID, we won't create a new drawable, we'll use the
			// one that has been created and attached to the object already
			//
			if( objectID != INVALID_ID )
			{
				Object *object = TheGameLogic->findObjectByID( objectID );

				// sanity
				if( object == nullptr )
				{

					DEBUG_CRASH(( "GameClient::xfer - Cannot find object '%d' that is supposed to be attached to this drawable '%s'",
												objectID, thingTemplate->getName().str() ));
					throw SC_INVALID_DATA;

				}

				// get the drawable from the object
				draw = object->getDrawable();
				if( draw == nullptr )
				{

					DEBUG_CRASH(( "GameClient::xfer - There is no drawable attached to the object '%s' (%d) and there should be",
												object->getTemplate()->getName().str(), object->getID() ));
					throw SC_INVALID_DATA;

				}

				// srj sez: some objects (eg, diguised bombtrucks) may have an "abnormal" drawable. so check.
				//
				// note carefully: we do NOT want to use isEquivalentTo() here, because different object reskins
				// SHOULD count as different templates for our purposes here (which are purely visual). however, we
				// do need to compare getFinalOverride, because draw->getTemplate() is always gonna return the final
				// override, while TheThingFactory->findTemplate does not.
				//
				const ThingTemplate* drawTemplate = draw->getTemplate();
				if (drawTemplate->getFinalOverride() != thingTemplate->getFinalOverride())
				{
					destroyDrawable( draw );
					draw = TheThingFactory->newDrawable( thingTemplate );
					TheGameLogic->bindObjectAndDrawable(object, draw);
				}

			}
			else
			{

				//
				// there was no object attached to this drawable when we saved, we need to create a
				// whole brand new drawable now
				//
				draw = TheThingFactory->newDrawable( thingTemplate );

				// sanity
				if( draw == nullptr )
				{

					DEBUG_CRASH(( "GameClient::xfer - Unable to create drawable for '%s'",
												thingTemplate->getName().str() ));
					throw SC_INVALID_DATA;

				}

			}

			// xfer the drawable data
			xfer->xferSnapshot( draw );

			// end block (not necessary since this is a no-op but symettrically nice)
			xfer->endBlock();

		}

	}

	// xfer the in-game mission briefing history list
	if (version >= 2)
	{
		if( xfer->getXferMode() == XFER_SAVE )
		{
			BriefingList *bList = GetBriefingTextList();
			Int numEntries = bList->size();
			xfer->xferInt(&numEntries);
			DEBUG_LOG(("Saving %d briefing lines", numEntries));
			for (BriefingList::const_iterator bIt = bList->begin(); bIt != bList->end(); ++bIt)
			{
				AsciiString tempStr = *bIt;
				DEBUG_LOG(("'%s'", tempStr.str()));
				xfer->xferAsciiString(&tempStr);
			}
		}
		else // XFER_LOAD
		{
			Int numEntries = 0;
			xfer->xferInt(&numEntries);
			DEBUG_LOG(("Loading %d briefing lines", numEntries));
			UpdateDiplomacyBriefingText(AsciiString::TheEmptyString, TRUE); // clear out briefing list first
			while (numEntries-- > 0)
			{
				AsciiString tempStr;
				xfer->xferAsciiString(&tempStr);
				DEBUG_LOG(("'%s'", tempStr.str()));
				UpdateDiplomacyBriefingText(tempStr, FALSE);
			}
		}
	}

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void GameClient::loadPostProcess()
{

	//
	// due to the fact that during the load process we have called newDrawable for drawables
	// without objects, and then overwrote their ids with data from the save file, our allocator
	// id may be far higher than it needs to be.  We'll pull it back down as low as we can
	//
	Drawable *draw;
	for( draw = getDrawableList(); draw; draw = draw->getNextDrawable() )
		if( draw->getID() >= m_nextDrawableID )
			m_nextDrawableID = (DrawableID)((UnsignedInt)draw->getID() + 1);

}

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void GameClient::crc( Xfer *xfer )
{

}
