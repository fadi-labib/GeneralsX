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

// GameEngine.cpp /////////////////////////////////////////////////////////////////////////////////
// Implementation of the Game Engine singleton
// Author: Michael S. Booth, April 2001

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#ifdef __EMSCRIPTEN__
#include <emscripten.h>  // emscripten_set_main_loop for the browser render loop
#include <exception>     // std::exception in the wasm frame's resilient catch
#include <dx8wasm/telemetry.h>
// GeneralsX @build dx8wasm — per-frame timing for the web build. Two spans plus one
// gauge a frame at 60 FPS is 180 ring records/second against a 1024-record ring
// drained at 1 Hz: fits with headroom, and dx8wasm_tel_dropped() reports it if it
// ever stops fitting.
#define GX_TEL_SPAN_BEGIN() const double gx_t0 = emscripten_get_now()
#define GX_TEL_SPAN_END(name) dx8wasm_tel_span(name, emscripten_get_now() - gx_t0)
#define GX_TEL_GAUGE(name, value) dx8wasm_tel_gauge(name, (double)(value))
#else
#define GX_TEL_SPAN_BEGIN() do {} while (0)
#define GX_TEL_SPAN_END(name) do {} while (0)
#define GX_TEL_GAUGE(name, value) do {} while (0)
#endif

#include "Common/ActionManager.h"
#include "Common/AudioAffect.h"
#include "Common/BuildAssistant.h"
#include "Common/CRCDebug.h"
#include "Common/FramePacer.h"
#include "Common/Radar.h"
#include "Common/PlayerTemplate.h"
#include "Common/Team.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "Common/GameAudio.h"
#include "Common/GameEngine.h"
#include "Common/INI.h"
#include "Common/INIException.h"
#include "Common/MessageStream.h"
#include "Common/ThingFactory.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/ArchiveFileSystem.h"
#include "Common/LocalFileSystem.h"
#include "Common/GlobalData.h"
#include "Common/PerfTimer.h"
#include "Common/RandomValue.h"
#include "Common/NameKeyGenerator.h"
#include "Common/ModuleFactory.h"
#include "Common/Debug.h"
#include "Common/GameState.h"
#include "Common/GameStateMap.h"
#include "Common/Science.h"
#include "Common/FunctionLexicon.h"
#include "Common/CommandLine.h"
#include "Common/DamageFX.h"
#include "Common/MultiplayerSettings.h"
#include "Common/Recorder.h"
#include "Common/SpecialPower.h"
#include "Common/TerrainTypes.h"
#include "Common/Upgrade.h"
#include "Common/OptionPreferences.h"
#include "Common/Xfer.h"
#include "Common/XferCRC.h"
#include "Common/GameLOD.h"
#include "Common/Registry.h"
#include "Common/GameCommon.h"	// FOR THE ALLOW_DEBUG_CHEATS_IN_RELEASE #define

#include "GameLogic/Armor.h"
#include "GameLogic/AI.h"
#include "GameLogic/CaveSystem.h"
#include "GameLogic/CrateSystem.h"
#include "GameLogic/Damage.h"
#include "GameLogic/VictoryConditions.h"
#include "GameLogic/ObjectCreationList.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/Locomotor.h"
#include "GameLogic/RankInfo.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/SidesList.h"

#include "GameClient/ClientInstance.h"
#include "GameClient/FXList.h"
#include "GameClient/GameClient.h"
#include "GameClient/Keyboard.h"
#include "GameClient/Shell.h"
#include "GameClient/GameText.h"
#include "GameClient/ParticleSys.h"
#include "GameClient/Water.h"
#include "GameClient/TerrainRoads.h"
#include "GameClient/MetaEvent.h"
#include "GameClient/MapUtil.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/Drawable.h"
#include "GameClient/GUICallbacks.h"

#include "GameNetwork/GameInfo.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/WOLBrowser/WebBrowser.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/GameSpy/GameResultsThread.h"

#include "Common/version.h"


//-------------------------------------------------------------------------------------------------

#ifdef DEBUG_CRC
class DeepCRCSanityCheck : public SubsystemInterface
{
public:
	DeepCRCSanityCheck() {}
	virtual ~DeepCRCSanityCheck() {}

	virtual void init() {}
	virtual void reset();
	virtual void update() {}

protected:
};

DeepCRCSanityCheck *TheDeepCRCSanityCheck = nullptr;

void DeepCRCSanityCheck::reset()
{
	static Int timesThrough = 0;
	static UnsignedInt lastCRC = 0;

	AsciiString fname;
	fname.format("%sCRCAfter%dMaps.dat", TheGlobalData->getPath_UserData().str(), timesThrough);
	UnsignedInt thisCRC = TheGameLogic->getCRC( CRC_RECALC, fname );

	DEBUG_LOG(("DeepCRCSanityCheck: CRC is %X", thisCRC));
	DEBUG_ASSERTCRASH(timesThrough == 0 || thisCRC == lastCRC,
		("CRC after reset did not match beginning CRC!\nNetwork games won't work after this.\nOld: 0x%8.8X, New: 0x%8.8X",
		lastCRC, thisCRC));
	lastCRC = thisCRC;

	timesThrough++;
}
#endif // DEBUG_CRC

//-------------------------------------------------------------------------------------------------
/// The GameEngine singleton instance
GameEngine *TheGameEngine = nullptr;

//-------------------------------------------------------------------------------------------------
SubsystemInterfaceList* TheSubsystemList = nullptr;

//-------------------------------------------------------------------------------------------------
template<class SUBSYSTEM>
void initSubsystem(
	SUBSYSTEM*& sysref,
	AsciiString name,
	SUBSYSTEM* sys,
	Xfer *pXfer,
	const char* path1 = nullptr,
	const char* path2 = nullptr)
{
	sysref = sys;
	TheSubsystemList->initSubsystem(sys, path1, path2, pXfer, name);
}

//-------------------------------------------------------------------------------------------------
extern HINSTANCE ApplicationHInstance;  ///< our application instance
// TheSuperHackers @build fighter19 11/02/2026 COM module (Windows-only)
#ifdef _WIN32
extern CComModule _Module;
#endif

//-------------------------------------------------------------------------------------------------
static void updateTGAtoDDS();

//-------------------------------------------------------------------------------------------------
static void updateWindowTitle()
{
	// TheSuperHackers @tweak Now prints product and version information in the Window title.

	DEBUG_ASSERTCRASH(TheVersion != nullptr, ("TheVersion is null"));
	DEBUG_ASSERTCRASH(TheGameText != nullptr, ("TheGameText is null"));

	UnicodeString title;

	if (rts::ClientInstance::getInstanceId() > 1u)
	{
		UnicodeString str;
		str.format(L"Instance:%.2u", rts::ClientInstance::getInstanceId());
		title.concat(str);
	}

	UnicodeString productString = TheVersion->getUnicodeProductString();

	if (!productString.isEmpty())
	{
		if (!title.isEmpty())
			title.concat(L" ");
		title.concat(productString);
	}

#if RTS_GENERALS
	const WideChar* defaultGameTitle = L"Command and Conquer Generals";
#elif RTS_ZEROHOUR
	const WideChar* defaultGameTitle = L"Command and Conquer Generals Zero Hour";
#endif
	UnicodeString gameTitle = TheGameText->FETCH_OR_SUBSTITUTE("GUI:Command&ConquerGenerals", defaultGameTitle);

	if (!gameTitle.isEmpty())
	{
		UnicodeString gameTitleFinal;
		UnicodeString gameVersion = TheVersion->getUnicodeVersion();

		if (productString.isEmpty())
		{
			gameTitleFinal = gameTitle;
		}
		else
		{
			UnicodeString gameTitleFormat = TheGameText->FETCH_OR_SUBSTITUTE("Version:GameTitle", L"for %ls");
			gameTitleFinal.format(gameTitleFormat.str(), gameTitle.str());
		}

		if (!title.isEmpty())
			title.concat(L" ");
		title.concat(gameTitleFinal.str());
		title.concat(L" ");
		title.concat(gameVersion.str());
	}

	if (!title.isEmpty())
	{
		AsciiString titleA;
		titleA.translate(title);	//get ASCII version for Win 9x

		extern HWND ApplicationHWnd;  ///< our application window handle
		if (ApplicationHWnd) {
// TheSuperHackers @build fighter19 11/02/2026 SetWindowText is Windows-only
#ifdef _WIN32
			//Set it twice because Win 9x does not support SetWindowTextW.
			::SetWindowText(ApplicationHWnd, titleA.str());
			::SetWindowTextW(ApplicationHWnd, title.str());
#else
			// Linux: SDL3 handles window title (set via SDL_SetWindowTitle if needed)
#endif
		}
	}
}

//-------------------------------------------------------------------------------------------------
GameEngine::GameEngine()
{
	// initialize to non garbage values
	m_logicTimeAccumulator = 0.0f;
	m_quitting = FALSE;
	m_isActive = FALSE;

// TheSuperHackers @build fighter19 11/02/2026 COM initialization (Windows-only)
#ifdef _WIN32
	_Module.Init(nullptr, ApplicationHInstance, nullptr);
#endif
}

//-------------------------------------------------------------------------------------------------
GameEngine::~GameEngine()
{
	//extern std::vector<std::string>	preloadTextureNamesGlobalHack;
	//preloadTextureNamesGlobalHack.clear();

	delete TheMapCache;
	TheMapCache = nullptr;

//	delete TheShell;
//	TheShell = nullptr;

	TheGameResultsQueue->endThreads();

	// TheSuperHackers @fix helmutbuhler 03/06/2025
	// Reset all subsystems before deletion to prevent crashing due to cross dependencies.
	reset();

	TheSubsystemList->shutdownAll();
	delete TheSubsystemList;
	TheSubsystemList = nullptr;

	delete TheSkirmishGameInfo;
	TheSkirmishGameInfo = nullptr;

	delete TheChallengeGameInfo;
	TheChallengeGameInfo = nullptr;

	delete TheNetwork;
	TheNetwork = nullptr;

	delete TheCommandList;
	TheCommandList = nullptr;

	delete TheNameKeyGenerator;
	TheNameKeyGenerator = nullptr;

	delete TheFileSystem;
	TheFileSystem = nullptr;

	delete TheGameLODManager;
	TheGameLODManager = nullptr;

	Drawable::killStaticImages();

// TheSuperHackers @build fighter19 11/02/2026 COM termination (Windows-only)
#ifdef _WIN32
	_Module.Term();
#endif

#ifdef PERF_TIMERS
	PerfGather::termPerfDump();
#endif
}

//-------------------------------------------------------------------------------------------------
Bool GameEngine::isTimeFrozen()
{
	// TheSuperHackers @fix The time can no longer be frozen in Network games. It would disconnect the player.
	if (TheNetwork != nullptr)
		return false;

	if (TheTacticalView != nullptr)
	{
		if (TheTacticalView->isTimeFrozen() && !TheTacticalView->isCameraMovementFinished())
			return true;
	}

	if (TheScriptEngine != nullptr)
	{
		if (TheScriptEngine->isTimeFrozenDebug() || TheScriptEngine->isTimeFrozenScript())
			return true;
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
Bool GameEngine::isGameHalted()
{
	if (TheNetwork != nullptr)
	{
		if (TheNetwork->isStalling())
			return true;
	}
	else
	{
		if (TheGameLogic != nullptr && TheGameLogic->isGamePaused())
			return true;
	}

	return false;
}

/** -----------------------------------------------------------------------------------------------
 * Initialize the game engine by initializing the GameLogic and GameClient.
 */
void GameEngine::init()
{
	try {
		//create an INI object to use for loading stuff
		INI ini;

#ifdef DEBUG_LOGGING
		if (TheVersion)
		{
			DEBUG_LOG(("================================================================================"));
			DEBUG_LOG(("Generals version %s", TheVersion->getAsciiVersion().str()));
			DEBUG_LOG(("Build date: %s", TheVersion->getAsciiBuildTime().str()));
			DEBUG_LOG(("Build location: %s", TheVersion->getAsciiBuildLocation().str()));
			DEBUG_LOG(("Build user: %s", TheVersion->getAsciiBuildUser().str()));
			DEBUG_LOG(("Build git revision: %s", TheVersion->getAsciiGitCommitCount().str()));
			DEBUG_LOG(("Build git version: %s", TheVersion->getAsciiGitTagOrHash().str()));
			DEBUG_LOG(("Build git commit time: %s", TheVersion->getAsciiGitCommitTime().str()));
			DEBUG_LOG(("Build git commit author: %s", Version::getGitCommitAuthorName()));
			DEBUG_LOG(("================================================================================"));
		}
#endif

	#if defined(PERF_TIMERS) || defined(DUMP_PERF_STATS)
		DEBUG_LOG(("Calculating CPU frequency for performance timers."));
		InitPrecisionTimer();
	#endif
	#ifdef PERF_TIMERS
		PerfGather::initPerfDump("AAAPerfStats", PerfGather::PERF_NETTIME);
	#endif




	#ifdef DUMP_PERF_STATS////////////////////////////////////////////////////////////
	__int64 startTime64;//////////////////////////////////////////////////////////////
	__int64 endTime64,freq64;///////////////////////////////////////////////////////////
	GetPrecisionTimerTicksPerSec(&freq64);///////////////////////////////////////////////
	GetPrecisionTimer(&startTime64);////////////////////////////////////////////////////
  char Buf[256];//////////////////////////////////////////////////////////////////////
	#endif//////////////////////////////////////////////////////////////////////////////


		TheSubsystemList = MSGNEW("GameEngineSubsystem") SubsystemInterfaceList;

		TheSubsystemList->addSubsystem(this);

		// initialize the random number system
		InitRandom();

		// Create the low-level file system interface
		TheFileSystem = createFileSystem();

		// not part of the subsystem list, because it should normally never be reset!
		TheNameKeyGenerator = MSGNEW("GameEngineSubsystem") NameKeyGenerator;
		TheNameKeyGenerator->init();


    	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheNameKeyGenerator  = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		// not part of the subsystem list, because it should normally never be reset!
		TheCommandList = MSGNEW("GameEngineSubsystem") CommandList;
		TheCommandList->init();

    	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheCommandList  = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		XferCRC xferCRC;
		xferCRC.open("lightCRC");


		initSubsystem(TheLocalFileSystem, "TheLocalFileSystem", createLocalFileSystem(), nullptr);


    	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheLocalFileSystem  = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		initSubsystem(TheArchiveFileSystem, "TheArchiveFileSystem", createArchiveFileSystem(), nullptr); // this MUST come after TheLocalFileSystem creation

    	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheArchiveFileSystem  = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		DEBUG_ASSERTCRASH(TheWritableGlobalData,("TheWritableGlobalData expected to be created"));
	initSubsystem(TheWritableGlobalData, "TheWritableGlobalData", TheWritableGlobalData, &xferCRC, "Data\\INI\\Default\\GameData", "Data\\INI\\GameData");
	TheWritableGlobalData->parseCustomDefinition();

	// GeneralsX @feature felipebraz 08/06/2026 Auto-create SagePatch.ini in user data dir with defaults.
	// This replaces the run.sh copy approach with engine-managed defaults.
	{
		AsciiString sagePatchPath = TheWritableGlobalData->getPath_UserData();
		sagePatchPath.concat("SagePatch.ini");

		if (!TheLocalFileSystem->doesFileExist(sagePatchPath.str()))
		{
			FILE *f = fopen(sagePatchPath.str(), "w");
			if (f)
			{
				fprintf(f,
				"; -----------------------------------------------------------------------------\n"
				"; SagePatch - Casual QoL overrides for GeneralsX\n"
				";\n"
				"; Loaded by the engine after the BIG-archived Data/INI/GameData.ini, so values\n"
				"; here override (not append to) the originals.\n"
				"; -----------------------------------------------------------------------------\n"
				"\n"
				"GameData\n"
				"  ; Slightly higher than vanilla (310); further out without seeing past the map border.\n"
				"  MaxCameraHeight = 350.0\n"
				"  ; Slightly lower than vanilla (120) so casual zoom-in feels useful.\n"
				"  MinCameraHeight = 100.0\n"
					"  ; Still soft-disabled so the user can push past max without a hard clamp.\n"
					"  EnforceMaxCameraHeight = No\n"
					"  ; Keyboard scroll - vanilla 0.5 is sluggish, double it.\n"
					"  KeyboardScrollSpeedFactor = 1.0\n"
					"  ; ~5% more terrain drawn at max zoom to fix terrain pop-in.\n"
					"  TerrainDrawDistanceScale = 1.05\n"
					// GeneralsX @tweak felipebraz 20/06/2026 Default render FPS limit to 60 FPS in SagePatch.ini
					"  UseFPSLimit = Yes\n"
					"  FramesPerSecondLimit = 60\n"
					"End\n"
				);
				fclose(f);
			}
		}

		if (TheLocalFileSystem->doesFileExist(sagePatchPath.str()))
		{
			// Check and migrate existing SagePatch.ini for 60 FPS
			FILE *f = fopen(sagePatchPath.str(), "rb");
			if (f)
			{
				fseek(f, 0, SEEK_END);
				long size = ftell(f);
				fseek(f, 0, SEEK_SET);
				char *buffer = new char[size + 1];
				fread(buffer, 1, size, f);
				buffer[size] = 0;
				fclose(f);

				if (!strstr(buffer, "FramesPerSecondLimit"))
				{
					char *endPos = strstr(buffer, "End");
					if (endPos != nullptr)
					{
						*endPos = '\0';
						FILE *fw = fopen(sagePatchPath.str(), "wb");
						if (fw)
						{
							fprintf(fw, "%s  ; Migrated 60 FPS defaults\n  UseFPSLimit = Yes\n  FramesPerSecondLimit = 60\nEnd\n", buffer);
							fclose(fw);
						}
					}
				}
				delete[] buffer;
			}

			ini.load(sagePatchPath, INI_LOAD_OVERWRITE, nullptr);
		}
	}

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After  TheWritableGlobalData = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////



	#if defined(RTS_DEBUG)
		// If we're in Debug, load the Debug settings as well.
		ini.loadFileDirectory( "Data\\INI\\GameDataDebug", INI_LOAD_OVERWRITE, nullptr );
	#endif

		// special-case: parse command-line parameters after loading global data
		CommandLine::parseCommandLineForEngineInit();

		TheArchiveFileSystem->loadMods();

		// doesn't require resets so just create a single instance here.
		TheGameLODManager = MSGNEW("GameEngineSubsystem") GameLODManager;
		TheGameLODManager->init();

		// after parsing the command line, we may want to perform dds stuff. Do that here.
		if (TheGlobalData->m_shouldUpdateTGAToDDS) {
			// update any out of date targas here.
			updateTGAtoDDS();
		}

		// read the water settings from INI (must do prior to initing GameClient, apparently)
		ini.loadFileDirectory( "Data\\INI\\Default\\Water", INI_LOAD_OVERWRITE, &xferCRC );
		ini.loadFileDirectory( "Data\\INI\\Water", INI_LOAD_OVERWRITE, &xferCRC );
		ini.loadFileDirectory( "Data\\INI\\Default\\Weather", INI_LOAD_OVERWRITE, &xferCRC );
		ini.loadFileDirectory( "Data\\INI\\Weather", INI_LOAD_OVERWRITE, &xferCRC );



	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After water INI's = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


#ifdef DEBUG_CRC
		initSubsystem(TheDeepCRCSanityCheck, "TheDeepCRCSanityCheck", MSGNEW("GameEngineSubystem") DeepCRCSanityCheck, nullptr);
#endif // DEBUG_CRC
		initSubsystem(TheGameText, "TheGameText", CreateGameTextInterface(), nullptr);
		updateWindowTitle();

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheGameText = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


#if RETAIL_COMPATIBLE_CRC
		if (xferCRC.getCRC() == 0xA1E7F8E6)
			TheNameKeyGenerator->verifyNameKeyID(1);
#endif

		initSubsystem(TheScienceStore,"TheScienceStore", MSGNEW("GameEngineSubsystem") ScienceStore(), &xferCRC, "Data\\INI\\Default\\Science", "Data\\INI\\Science");
		initSubsystem(TheMultiplayerSettings,"TheMultiplayerSettings", MSGNEW("GameEngineSubsystem") MultiplayerSettings(), &xferCRC, "Data\\INI\\Default\\Multiplayer", "Data\\INI\\Multiplayer");
		initSubsystem(TheTerrainTypes,"TheTerrainTypes", MSGNEW("GameEngineSubsystem") TerrainTypeCollection(), &xferCRC, "Data\\INI\\Default\\Terrain", "Data\\INI\\Terrain");
		initSubsystem(TheTerrainRoads,"TheTerrainRoads", MSGNEW("GameEngineSubsystem") TerrainRoadCollection(), &xferCRC, "Data\\INI\\Default\\Roads", "Data\\INI\\Roads");
		initSubsystem(TheGlobalLanguageData,"TheGlobalLanguageData",MSGNEW("GameEngineSubsystem") GlobalLanguage, nullptr); // must be before the game text
		TheGlobalLanguageData->parseCustomDefinition();
	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheGlobalLanguageData = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////
		initSubsystem(TheAudio,"TheAudio", createAudioManager(TheGlobalData->m_headless), nullptr);
#ifndef __EMSCRIPTEN__
		// GeneralsX @build dx8wasm - audio is stubbed on wasm (MiniAudio, no music
		// yet), so "music not loaded" is expected, not a fatal condition. Guarding
		// this prevents the engine from quitting before the main loop starts.
		if (!TheAudio->isMusicAlreadyLoaded())
			setQuitting(TRUE);
#endif

#if RTS_ZEROHOUR && RETAIL_COMPATIBLE_CRC
		TheNameKeyGenerator->syncNameKeyID();
#endif

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheAudio = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		initSubsystem(TheFunctionLexicon,"TheFunctionLexicon", createFunctionLexicon(), nullptr);
		initSubsystem(TheModuleFactory,"TheModuleFactory", createModuleFactory(), nullptr);
		initSubsystem(TheMessageStream,"TheMessageStream", createMessageStream(), nullptr);
		initSubsystem(TheSidesList,"TheSidesList", MSGNEW("GameEngineSubsystem") SidesList(), nullptr);
		initSubsystem(TheCaveSystem,"TheCaveSystem", MSGNEW("GameEngineSubsystem") CaveSystem(), nullptr);
		initSubsystem(TheRankInfoStore,"TheRankInfoStore", MSGNEW("GameEngineSubsystem") RankInfoStore(), &xferCRC, nullptr, "Data\\INI\\Rank");
		initSubsystem(ThePlayerTemplateStore,"ThePlayerTemplateStore", MSGNEW("GameEngineSubsystem") PlayerTemplateStore(), &xferCRC, "Data\\INI\\Default\\PlayerTemplate", "Data\\INI\\PlayerTemplate");
		initSubsystem(TheParticleSystemManager,"TheParticleSystemManager", createParticleSystemManager(TheGlobalData->m_headless), nullptr);

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheParticleSystemManager = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		initSubsystem(TheFXListStore,"TheFXListStore", MSGNEW("GameEngineSubsystem") FXListStore(), &xferCRC, "Data\\INI\\Default\\FXList", "Data\\INI\\FXList");
		initSubsystem(TheWeaponStore,"TheWeaponStore", MSGNEW("GameEngineSubsystem") WeaponStore(), &xferCRC, nullptr, "Data\\INI\\Weapon");
		initSubsystem(TheObjectCreationListStore,"TheObjectCreationListStore", MSGNEW("GameEngineSubsystem") ObjectCreationListStore(), &xferCRC, "Data\\INI\\Default\\ObjectCreationList", "Data\\INI\\ObjectCreationList");
		initSubsystem(TheLocomotorStore,"TheLocomotorStore", MSGNEW("GameEngineSubsystem") LocomotorStore(), &xferCRC, nullptr, "Data\\INI\\Locomotor");
		initSubsystem(TheSpecialPowerStore,"TheSpecialPowerStore", MSGNEW("GameEngineSubsystem") SpecialPowerStore(), &xferCRC, "Data\\INI\\Default\\SpecialPower", "Data\\INI\\SpecialPower");
		initSubsystem(TheDamageFXStore,"TheDamageFXStore", MSGNEW("GameEngineSubsystem") DamageFXStore(), &xferCRC, nullptr, "Data\\INI\\DamageFX");
		initSubsystem(TheArmorStore,"TheArmorStore", MSGNEW("GameEngineSubsystem") ArmorStore(), &xferCRC, nullptr, "Data\\INI\\Armor");
		initSubsystem(TheBuildAssistant,"TheBuildAssistant", MSGNEW("GameEngineSubsystem") BuildAssistant, nullptr);


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheBuildAssistant = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////



		initSubsystem(TheThingFactory,"TheThingFactory", createThingFactory(), &xferCRC, "Data\\INI\\Default\\Object", "Data\\INI\\Object");

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheThingFactory = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


#if RETAIL_COMPATIBLE_CRC
		if (xferCRC.getCRC() == 0x6209AF6E)
			TheNameKeyGenerator->verifyNameKeyID(2265);
#endif

		initSubsystem(TheUpgradeCenter,"TheUpgradeCenter", MSGNEW("GameEngineSubsystem") UpgradeCenter, &xferCRC, "Data\\INI\\Default\\Upgrade", "Data\\INI\\Upgrade");
		initSubsystem(TheGameClient,"TheGameClient", createGameClient(), nullptr);


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheGameClient = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		initSubsystem(TheAI,"TheAI", MSGNEW("GameEngineSubsystem") AI(), &xferCRC,  "Data\\INI\\Default\\AIData", "Data\\INI\\AIData");
		initSubsystem(TheGameLogic,"TheGameLogic", createGameLogic(), nullptr);
		initSubsystem(TheTeamFactory,"TheTeamFactory", MSGNEW("GameEngineSubsystem") TeamFactory(), nullptr);
		initSubsystem(TheCrateSystem,"TheCrateSystem", MSGNEW("GameEngineSubsystem") CrateSystem(), &xferCRC, "Data\\INI\\Default\\Crate", "Data\\INI\\Crate");
		initSubsystem(ThePlayerList,"ThePlayerList", MSGNEW("GameEngineSubsystem") PlayerList(), nullptr);
		initSubsystem(TheRecorder,"TheRecorder", createRecorder(), nullptr);
		initSubsystem(TheRadar,"TheRadar", createRadar(TheGlobalData->m_headless), nullptr);
		initSubsystem(TheVictoryConditions,"TheVictoryConditions", createVictoryConditions(), nullptr);



	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheVictoryConditions = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		AsciiString fname;
		fname.format("Data\\%s\\CommandMap", GetRegistryLanguage().str());
		initSubsystem(TheMetaMap,"TheMetaMap", MSGNEW("GameEngineSubsystem") MetaMap(), nullptr, fname.str(), "Data\\INI\\CommandMap");

#if defined(RTS_DEBUG)
		ini.loadFileDirectory("Data\\INI\\CommandMapDebug", INI_LOAD_MULTIFILE, nullptr);
#endif

#if defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
		ini.loadFileDirectory("Data\\INI\\CommandMapDemo", INI_LOAD_MULTIFILE, nullptr);
#endif

		TheMetaMap->generateMetaMap();
		TheMetaMap->verifyMetaMap();


		initSubsystem(TheActionManager,"TheActionManager", MSGNEW("GameEngineSubsystem") ActionManager(), nullptr);
		//initSubsystem((CComObject<WebBrowser> *)TheWebBrowser,"(CComObject<WebBrowser> *)TheWebBrowser", (CComObject<WebBrowser> *)createWebBrowser(), nullptr);
		initSubsystem(TheGameStateMap,"TheGameStateMap", MSGNEW("GameEngineSubsystem") GameStateMap, nullptr );
		initSubsystem(TheGameState,"TheGameState", MSGNEW("GameEngineSubsystem") GameState, nullptr );

		// Create the interface for sending game results
		initSubsystem(TheGameResultsQueue,"TheGameResultsQueue", GameResultsInterface::createNewGameResultsInterface(), nullptr);


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheGameResultsQueue = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		xferCRC.close();
		TheWritableGlobalData->m_iniCRC = xferCRC.getCRC();
		DEBUG_LOG(("INI CRC is 0x%8.8X", TheGlobalData->m_iniCRC));

		TheSubsystemList->postProcessLoadAll();

		// GeneralsX @bugfix Copilot 11/05/2026 Prevent uncapped render when FPS limiter is enabled but no valid limit value was loaded.
		if (TheGlobalData->m_useFpsLimit && TheGlobalData->m_framesPerSecondLimit <= 0)
		{
			TheWritableGlobalData->m_framesPerSecondLimit = BaseFps;
		}

		TheFramePacer->setFramesPerSecondLimit(TheGlobalData->m_framesPerSecondLimit);

		TheAudio->setOn(TheGlobalData->m_audioOn && TheGlobalData->m_musicOn, AudioAffect_Music);
		TheAudio->setOn(TheGlobalData->m_audioOn && TheGlobalData->m_soundsOn, AudioAffect_Sound);
		TheAudio->setOn(TheGlobalData->m_audioOn && TheGlobalData->m_sounds3DOn, AudioAffect_Sound3D);
		TheAudio->setOn(TheGlobalData->m_audioOn && TheGlobalData->m_speechOn, AudioAffect_Speech);

		// We're not in a network game yet, so set the network singleton to nullptr.
		TheNetwork = nullptr;

		//Create a default ini file for options if it doesn't already exist.
		//OptionPreferences prefs( TRUE );

		// If we turn m_quitting to FALSE here, then we throw away any requests to quit that
		// took place during loading. :-\ - jkmcd
		// If this really needs to take place, please make sure that pressing cancel on the audio
		// load music dialog will still cause the game to quit.
		// m_quitting = FALSE;

		// initialize the MapCache
		TheMapCache = MSGNEW("GameEngineSubsystem") MapCache;
		TheMapCache->updateCache();


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheMapCache->updateCache = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		if (TheGlobalData->m_buildMapCache)
		{
			// just quit, since the map cache has already updated
			//populateMapListbox(nullptr, true, true);
			m_quitting = TRUE;
		}

		// load the initial shell screen
		TheShell->push( "Menus/MainMenu.wnd" );

#ifdef __EMSCRIPTEN__
		// GeneralsX @build dx8wasm - skip the EA logo + sizzle intro on the normal (menu)
		// boot. The Bink intro movies aren't packaged on web (MOVIES=0), and the intro
		// state machine (GameClient::update) otherwise parks in the movie branch for a long
		// time before showShellMap()/showShell() run. Clearing PlayIntro/PlaySizzle makes
		// the "after intro" path run on the first frame (see the !m_playIntro -> m_afterIntro
		// assignment below), so the main menu + shell-map backdrop appear immediately. The
		// -file skirmish path below disables these separately for its own reasons.
		if (TheGlobalData->m_initialFile.isEmpty())
		{
			TheWritableGlobalData->m_playIntro = FALSE;
			TheWritableGlobalData->m_playSizzle = FALSE;
		}
#endif

		// This allows us to run a map from the command line
		if (TheGlobalData->m_initialFile.isEmpty() == FALSE)
		{
			AsciiString fname = TheGlobalData->m_initialFile;
			fname.toLower();

			if (fname.endsWithNoCase(".map"))
			{
				TheWritableGlobalData->m_shellMapOn = FALSE;
				TheWritableGlobalData->m_playIntro = FALSE;

#ifdef __EMSCRIPTEN__
				// Skip the logo/sizzle movie + "after intro" shell path entirely; on
				// wasm the Bink stub never finishes, which parks GameClient::update in
				// the movie branch (line "if(m_playIntro||m_afterIntro){DRAW;return;}")
				// so the in-game world is never drawn. Go straight to gameplay.
				TheWritableGlobalData->m_playSizzle = FALSE;
				TheWritableGlobalData->m_afterIntro = FALSE;

				// GeneralsX @build dx8wasm - `-campaign` runs the map as a SINGLE-PLAYER
				// mission (its own scripts, objectives and scripted opponents) rather than
				// the synthetic skirmish below. This is the same start the desktop build uses
				// for -file, and it exists because campaign is otherwise unreachable for
				// testing: it sits behind shell menu buttons, and synthetic clicks register
				// hover without activating SDL buttons.
				//   -file Maps/USA01/USA01.map -campaign
				// -aidifficulty still applies, mapping to the mission difficulty.
				if (TheGlobalData->m_wasmCampaign)
				{
					const Int cDiff = TheGlobalData->m_wasmAIDifficulty < 0 ? 1 : TheGlobalData->m_wasmAIDifficulty;
					const GameDifficulty campDiff =
						(cDiff >= 2) ? DIFFICULTY_HARD : (cDiff == 1) ? DIFFICULTY_NORMAL : DIFFICULTY_EASY;
					MAIN_THREAD_EM_ASM({ console.log('[CAMPAIGN] single-player mission, difficulty=' + $0); }, cDiff);

					TheWritableGlobalData->m_pendingFile = TheGlobalData->m_initialFile;
					TheWritableGlobalData->m_mapName = TheGlobalData->m_initialFile;
					GameMessage *cmsg = TheMessageStream->appendMessage( GameMessage::MSG_NEW_GAME );
					cmsg->appendIntegerArgument(GAME_SINGLE_PLAYER);
					cmsg->appendIntegerArgument(campDiff);
					cmsg->appendIntegerArgument(0);
					InitRandom(0);
				}
				else
				{
				// GeneralsX @build dx8wasm - a bare -file launch loads the map with no
				// armies (GAME_SINGLE_PLAYER + null TheGameInfo). Build a minimal
				// 2-player skirmish (human USA vs Easy-AI China) so units actually
				// spawn and the game is playable. Sequence adapted from the Start path
				// in SkirmishGameOptionsMenu.cpp (init/reset/enterGame, two slots,
				// startGame, MSG_NEW_GAME(GAME_SKIRMISH)).
				// The full map cache is normally populated when a map-select menu
				// opens; at engine-init launch time only the static MapCache.ini
				// subset is loaded, so scan now (needed for start-position waypoints).
				if (TheMapCache)
					TheMapCache->updateCache();

				// The bare -file launch has no skirmish UI, so computer players are otherwise
				// created as solo/scripted AI (AIPlayer) that never build a base -> the opponent
				// just sits. Force skirmish AI (AISkirmishPlayer) so it builds and fights. This is
				// exactly what AIData's ForceSkirmishAI flag is for ("...until the skirmish ui is
				// done"). getAiData() is const; cast to set the one dev flag.
				if (TheAI && TheAI->getAiData())
					const_cast<TAiData*>(TheAI->getAiData())->m_forceSkirmishAI = TRUE;

				if (!TheSkirmishGameInfo)
					TheSkirmishGameInfo = NEW SkirmishGameInfo;
				TheSkirmishGameInfo->init();
				TheSkirmishGameInfo->clearSlotList();
				TheSkirmishGameInfo->reset();
				TheSkirmishGameInfo->setLocalIP(TheSkirmishGameInfo->getSlot(0)->getIP());
				TheSkirmishGameInfo->enterGame();

				const Int usaTmpl   = ThePlayerTemplateStore->getTemplateNumByName("FactionAmerica");
				const Int chinaTmpl = ThePlayerTemplateStore->getTemplateNumByName("FactionChina");

				GameSlot humanSlot;
				humanSlot.setState(SLOT_PLAYER, UnicodeString(L"Player"));
				humanSlot.setColor(0);
				humanSlot.setPlayerTemplate(usaTmpl);
				humanSlot.setStartPos(0);       // explicit valid pos avoids the random-assign while-loops
				humanSlot.setTeamNumber(-1);    // -1 => no team (free-for-all: they fight)
				TheSkirmishGameInfo->setSlot(0, humanSlot);

				// -stress forces Brutal AI + huge cash for the headless perf repro. Otherwise
				// the AI level comes from -aidifficulty (easy|medium|hard), default Easy. The
				// skirmish setup UI isn't wired on web yet, so this launcher flag is how the
				// player picks difficulty; it drives both the AI slot skill and the game
				// difficulty (which scales AI resource/handicap bonuses).
				const Bool stress = TheGlobalData->m_wasmStressSkirmish;
				const Int aiDiff = stress ? 2
					: (TheGlobalData->m_wasmAIDifficulty < 0 ? 0 : TheGlobalData->m_wasmAIDifficulty);
				const SlotState aiState =
					(aiDiff >= 2) ? SLOT_BRUTAL_AI : (aiDiff == 1) ? SLOT_MED_AI : SLOT_EASY_AI;
				const GameDifficulty gameDiff =
					(aiDiff >= 2) ? DIFFICULTY_HARD : (aiDiff == 1) ? DIFFICULTY_NORMAL : DIFFICULTY_EASY;
				MAIN_THREAD_EM_ASM({ console.log('[SKIRMISH] AI difficulty=' + $0 + ' (0=easy,1=med,2=hard)'); }, aiDiff);
				GameSlot aiSlot;
				aiSlot.setState(aiState);
				aiSlot.setColor(1);
				aiSlot.setPlayerTemplate(chinaTmpl);
				aiSlot.setStartPos(1);
				aiSlot.setTeamNumber(-1);
				TheSkirmishGameInfo->setSlot(1, aiSlot);

				TheSkirmishGameInfo->setMap(TheGlobalData->m_initialFile);
				const MapMetaData *md = TheMapCache ? TheMapCache->findMap(TheGlobalData->m_initialFile) : NULL;
				if (md) { TheSkirmishGameInfo->setMapCRC(md->m_CRC); TheSkirmishGameInfo->setMapSize(md->m_filesize); }
				else    { TheSkirmishGameInfo->setMapCRC(0); TheSkirmishGameInfo->setMapSize(0); }
				TheSkirmishGameInfo->setSeed(1);
				Money stressCash; stressCash.setStartingCash(500000);
				TheSkirmishGameInfo->setStartingCash(stress ? stressCash : TheGlobalData->m_defaultStartingCash);
				TheSkirmishGameInfo->startGame(0);
				InitRandom(TheSkirmishGameInfo->getSeed());

				TheWritableGlobalData->m_mapName = TheGlobalData->m_initialFile;
				GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_NEW_GAME );
				msg->appendIntegerArgument(GAME_SKIRMISH);
				msg->appendIntegerArgument(gameDiff);
				msg->appendIntegerArgument(0);
				msg->appendIntegerArgument(60);   // FPS limit
				}   // end of the non-campaign (synthetic skirmish) branch
#else
				TheWritableGlobalData->m_pendingFile = TheGlobalData->m_initialFile;

				// shutdown the top, but do not pop it off the stack
	//			TheShell->hideShell();

				// send a message to the logic for a new game
				GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_NEW_GAME );
				msg->appendIntegerArgument(GAME_SINGLE_PLAYER);
				msg->appendIntegerArgument(DIFFICULTY_NORMAL);
				msg->appendIntegerArgument(0);
				InitRandom(0);
#endif
			}
		}

		//
		if (TheMapCache && TheGlobalData->m_shellMapOn)
		{
			AsciiString lowerName = TheGlobalData->m_shellMapName;
			lowerName.toLower();

			MapCache::const_iterator it = TheMapCache->find(lowerName);
			if (it == TheMapCache->end())
			{
				TheWritableGlobalData->m_shellMapOn = FALSE;
			}
		}

		// On a -file launch we skip the intro/shell and go straight to the game,
		// so don't force m_afterIntro back on (see the skirmish setup above).
		if(!TheGlobalData->m_playIntro && TheGlobalData->m_initialFile.isEmpty())
			TheWritableGlobalData->m_afterIntro = TRUE;

	}
	catch (ErrorCode ec)
	{
		if (ec == ERROR_INVALID_D3D)
		{
			RELEASE_CRASHLOCALIZED("ERROR:D3DFailurePrompt", "ERROR:D3DFailureMessage");
		}
	}
	catch (INIException e)
	{
		if (e.mFailureMessage)
			RELEASE_CRASH((e.mFailureMessage));
		else
			RELEASE_CRASH(("Uncaught Exception during initialization."));

	}
	catch (...)
	{
		RELEASE_CRASH(("Uncaught Exception during initialization."));
	}

	if(!TheGlobalData->m_playIntro && TheGlobalData->m_initialFile.isEmpty())
		TheWritableGlobalData->m_afterIntro = TRUE;

	resetSubsystems();

	HideControlBar();
}

/** -----------------------------------------------------------------------------------------------
	* Reset all necessary parts of the game engine to be ready to accept new game data
	*/
void GameEngine::reset()
{

	WindowLayout *background = TheWindowManager->winCreateLayout("Menus/BlankWindow.wnd");
	DEBUG_ASSERTCRASH(background,("We Couldn't Load Menus/BlankWindow.wnd"));
	background->hide(FALSE);
	background->bringForward();
	background->getFirstWindow()->winClearStatus(WIN_STATUS_IMAGE);
	Bool deleteNetwork = false;
	if (TheGameLogic->isInMultiplayerGame())
		deleteNetwork = true;

	resetSubsystems();

	if (deleteNetwork)
	{
		DEBUG_ASSERTCRASH(TheNetwork, ("Deleting null TheNetwork!"));
		delete TheNetwork;
		TheNetwork = nullptr;
	}
	if(background)
	{
		background->destroyWindows();
		deleteInstance(background);
		background = nullptr;
	}
}

/// -----------------------------------------------------------------------------------------------
void GameEngine::resetSubsystems()
{
	// TheSuperHackers @fix xezon 09/06/2025 Reset GameLogic first to purge all world objects early.
	// This avoids potentially catastrophic issues when objects and subsystems have cross dependencies.
	TheGameLogic->reset();

	TheSubsystemList->resetAll();
}

/// -----------------------------------------------------------------------------------------------
Bool GameEngine::canUpdateGameLogic()
{
	// This updates the paused game status of the game logic.
	TheGameLogic->preUpdate();

	TheFramePacer->setTimeFrozen(isTimeFrozen());
	TheFramePacer->setGameHalted(isGameHalted());

	if (TheNetwork != nullptr)
	{
		return canUpdateNetworkGameLogic();
	}
	else
	{
		return canUpdateRegularGameLogic();
	}
}

/// -----------------------------------------------------------------------------------------------
Bool GameEngine::canUpdateNetworkGameLogic()
{
	DEBUG_ASSERTCRASH(TheNetwork != nullptr, ("TheNetwork is null"));

	if (TheNetwork->isFrameDataReady())
	{
		// Important: The Network is definitely no longer stalling.
		TheFramePacer->setGameHalted(false);

		return true;
	}

	return false;
}

/// -----------------------------------------------------------------------------------------------
Bool GameEngine::canUpdateRegularGameLogic()
{
	const Bool enabled = TheFramePacer->isLogicTimeScaleEnabled();
	const Int logicTimeScaleFps = TheFramePacer->getLogicTimeScaleFps();
	const Int maxRenderFps = TheFramePacer->getFramesPerSecondLimit();

#if defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
	const Bool useFastMode = TheGlobalData->m_TiVOFastMode;
#else	//always allow this cheat key if we're in a replay game.
	const Bool useFastMode = TheGlobalData->m_TiVOFastMode && TheGameLogic->isInReplayGame();
#endif

	if (useFastMode || !enabled || logicTimeScaleFps >= maxRenderFps)
	{
		// Logic time scale is uncapped or larger equal Render FPS. Update straight away.
		return true;
	}
	else
	{
		// TheSuperHackers @tweak xezon 06/08/2025
		// The logic time step is now decoupled from the render update.
		const Real targetFrameTime = 1.0f / logicTimeScaleFps;
		m_logicTimeAccumulator += min(TheFramePacer->getUpdateTime(), targetFrameTime);

		if (m_logicTimeAccumulator >= targetFrameTime)
		{
			m_logicTimeAccumulator -= targetFrameTime;
			return true;
		}
	}

	return false;
}

/// -----------------------------------------------------------------------------------------------
DECLARE_PERF_TIMER(GameEngine_update)

#ifdef __EMSCRIPTEN__
static double g_wasmStressLogicMs = 0.0;   // last GameLogic::UPDATE() cost (ms), for the -stress perf log
#endif

/** -----------------------------------------------------------------------------------------------
 * Update the game engine by updating the GameClient and GameLogic singletons.
 */
void GameEngine::update()
{
#ifdef __EMSCRIPTEN__
	// GeneralsX @build dx8wasm - a -file launch whose map does not resolve renders a silent
	// black screen: the game starts, the HUD draws and the clock runs, but the world is empty.
	// That cost a long debugging session (the map path was doubled; see
	// ConvertShortMapPathToLongMapPath). Say so once instead, so the next wrong path is
	// obvious from the console rather than looking like a rendering bug.
	if (!TheGlobalData->m_initialFile.isEmpty())
	{
		static Int s_mapCheckFrames = 0;
		if (++s_mapCheckFrames == 600)   // ~10s in: well past map load, cheap one-shot
		{
			Int objs = 0;
			if (TheGameLogic)
				for (Object *o = TheGameLogic->getFirstObject(); o; o = o->getNextObject()) ++objs;
			if (objs == 0)
			{
				MAIN_THREAD_EM_ASM({
					console.error('[MAP] no objects in the world - map "' + UTF8ToString($0) +
						'" did not load (exists=' + $1 + '). Pass the SHORT map path, e.g. ' +
						'maps\\\\USA01.map, not maps\\\\USA01\\\\USA01.map.');
				},
					TheGlobalData->m_mapName.str(),
					(Int)(TheFileSystem ? TheFileSystem->doesFileExist(TheGlobalData->m_mapName.str()) : 0));
			}
		}
	}
#endif
	USE_PERF_TIMER(GameEngine_update)
	{
		{
			// VERIFY CRC needs to be in this code block.  Please to not pull TheGameLogic->update() inside this block.
			VERIFY_CRC

			TheRadar->UPDATE();

			/// @todo Move audio init, update, etc, into GameClient update

			TheAudio->UPDATE();
			// GeneralsX @build dx8wasm - per-frame client timing span; see GX_TEL_SPAN_* above.
			{ GX_TEL_SPAN_BEGIN(); TheGameClient->UPDATE(); GX_TEL_SPAN_END("frame.client"); }
			TheMessageStream->propagateMessages();

			if (TheNetwork != nullptr)
			{
				TheNetwork->UPDATE();
			}
		}

		// TheSuperHackers @info Ignores frozen time because the script engine needs updating in the logic update regardless.
		if (canUpdateGameLogic())
		{
#ifdef __EMSCRIPTEN__
			// Under -stress, isolate pure sim (logic) time from the render-inclusive frame so
			// CPU-logic growth stays visible even when software rendering dominates wall-clock.
			if (TheGlobalData && TheGlobalData->m_wasmStressSkirmish) {
				double lt0 = emscripten_get_now();
				{ GX_TEL_SPAN_BEGIN(); TheGameLogic->UPDATE(); GX_TEL_SPAN_END("frame.logic"); }
				g_wasmStressLogicMs = emscripten_get_now() - lt0;
			} else
#endif
			{ GX_TEL_SPAN_BEGIN(); TheGameLogic->UPDATE(); GX_TEL_SPAN_END("frame.logic"); }

			// GeneralsX @build dx8wasm — the simulation frame number, sampled after the
			// update that advanced it. Emitted once here rather than inside each branch
			// above so the -stress path and the normal path cannot diverge.
			//
			// Why a gauge and not a span attribute: this is the only signal that makes a
			// saved-game restore *provable* from outside the engine. m_frame only ever
			// advances during play, so a single backwards step is a reset — and a load is
			// the one thing in normal play that resets it. Every timing-derived
			// alternative was tried and lied (see generals-dx8wasm
			// docs/HARNESS-TRAPS.md §1): a restore is a blocking load with no
			// logic frames at all, not one slow frame, and the ring's own batching means
			// arrival gaps say nothing about when the engine stopped.
			GX_TEL_GAUGE("logic.frame", TheGameLogic->getFrame());

			if (!TheFramePacer->isTimeFrozen())
			{
				TheGameClient->step();
			}
		}
	}
}

// Horrible reference, but we really, really need to know if we are windowed.
extern bool DX8Wrapper_IsWindowed;
extern HWND ApplicationHWnd;

/** -----------------------------------------------------------------------------------------------
 * The "main loop" of the game engine. It will not return until the game exits.
 */
#ifdef __EMSCRIPTEN__
// Resolve the first available template from a candidate list (missing ones return null).
static const ThingTemplate* wasm_first_template(const char* const* names, int n)
{
	for (int i = 0; i < n; ++i) {
		const ThingTemplate* t = TheThingFactory->findTemplate(AsciiString(names[i]), FALSE);
		if (t) return t;
	}
	return nullptr;
}

// -stress: grow an army over time so per-frame CPU work accumulates, giving the in-game
// slowdown a headless repro without fragile UI automation. Spawns near the local player's
// Command Center in batches, capped. If a non-local playable opponent exists, spawns BOTH
// sides intermixed and close so auto-acquire drives real COMBAT (weapons/projectiles/death/
// collision) — the autonomous skirmish AI doesn't function in this port, so we script it.
// No-op unless -stress is set.
static void wasm_stress_spawn()
{
	if (!TheGlobalData || !TheGlobalData->m_wasmStressSkirmish) return;
	if (!TheGameLogic || !TheGameLogic->isInGame() || !ThePlayerList || !TheThingFactory) return;

	static UnsignedInt s_nextFrame = 300;   // let the base finish placing first
	static int s_spawned = 0;
	const int SPAWN_CAP = 400, BATCH = 20, PERIOD = 150;
	UnsignedInt f = TheGameLogic->getFrame();
	if (f < s_nextFrame || s_spawned >= SPAWN_CAP) return;
	s_nextFrame = f + PERIOD;

	Player* local = ThePlayerList->getLocalPlayer();
	if (!local) return;
	// Enemy = first non-local playable-side player (the opponent), for scripted combat.
	Player* enemy = nullptr;
	for (int i = 0; i < ThePlayerList->getPlayerCount(); ++i) {
		Player* p = ThePlayerList->getNthPlayer(i);
		if (p && p != local && p->isPlayableSide()) { enemy = p; break; }
	}
	// Force mutual hostility once so the two sides auto-acquire and actually fight
	// (the -file skirmish leaves them non-hostile, so units otherwise stand idle).
	static bool s_relSet = false;
	if (enemy && !s_relSet) {
		local->setPlayerRelationship(enemy, ENEMIES);
		enemy->setPlayerRelationship(local, ENEMIES);
		s_relSet = true;
	}
	// Reference position: the local player's first owned object (its Command Center).
	const Coord3D* ref = nullptr;
	for (Object* o = TheGameLogic->getFirstObject(); o; o = o->getNextObject())
		if (o->getControllingPlayer() == local) { ref = o->getPosition(); break; }
	if (!ref) return;

	const char* usaC[] = { "AmericaInfantryRanger", "AmericaVehicleHumvee", "AmericaInfantryMissileDefender" };
	const char* chiC[] = { "ChinaInfantryRedguard", "ChinaTankBattleMaster", "ChinaInfantryTankHunter" };
	const ThingTemplate* usaT = wasm_first_template(usaC, 3);
	const ThingTemplate* chiT = wasm_first_template(chiC, 3);
	if (!usaT) { fprintf(stderr, "[PERF warn] stress-spawn: no unit template found\n"); s_spawned = SPAWN_CAP; return; }

	for (int i = 0; i < BATCH && s_spawned < SPAWN_CAP; ++i, ++s_spawned) {
		// Alternate sides when we have an opponent + a foe template, so enemies are
		// interleaved and (at 40-unit spacing) within acquisition range -> they fight.
		const bool foe = enemy && chiT && (s_spawned & 1);
		Object* u = TheThingFactory->newObject(foe ? chiT : usaT, (foe ? enemy : local)->getDefaultTeam());
		if (!u) break;
		Coord3D p = *ref;
		p.x += (Real)((s_spawned % 20) * 40 - 400);
		p.y += (Real)(((s_spawned / 20) % 20) * 40 - 400);
		u->setPosition(&p);
	}
	fprintf(stderr, "[PERF warn] stress-spawn: total=%d combat=%s (gameFrame %u)\n",
		s_spawned, (enemy && chiT) ? "yes" : "no", f);
}

// One engine frame, driven by the browser's requestAnimationFrame. A blocking
// while(!quit) loop never returns to the event loop, so rendered frames are never
// composited to the canvas; emscripten_set_main_loop yields each frame so WebGL
// presents. See execute() below.
static void wasm_engine_frame()
{
	if (!TheGameEngine || TheGameEngine->getQuitting()) { emscripten_cancel_main_loop(); return; }
	wasm_stress_spawn();
	double pf_t0 = emscripten_get_now();
	// A single frame's exception must NOT kill the whole game. The old code called
	// emscripten_cancel_main_loop() here, which permanently tears down the rAF loop
	// (nulls MainLoop.scheduler) — the game goes black forever with no recovery, e.g.
	// after a transient hiccup on the first frame back from a backgrounded tab. Log and
	// skip this frame instead; a transient throw self-recovers next frame.
	try { TheGameEngine->update(); }
	catch (const std::exception& e) {
		fprintf(stderr, "[wasm] std::exception in engine update(): %s — skipping frame\n", e.what());
		return;
	}
	catch (...) {
		fprintf(stderr, "[wasm] unknown exception in engine update() — skipping frame\n");
		return;
	}
	TheFramePacer->update();
	// Under -stress (headless perf repro) report per-frame update() cost vs game frame,
	// so a rising trend during the AI battle is visible. Silent in normal runs.
	if (TheGlobalData && TheGlobalData->m_wasmStressSkirmish)
	{
		static int pf_n = 0; static double pf_acc = 0, pf_max = 0;
		double dt = emscripten_get_now() - pf_t0;
		pf_acc += dt; if (dt > pf_max) pf_max = dt; ++pf_n;
		if (pf_n >= 120)
		{
			int gf = (TheGameLogic && TheGameLogic->isInGame()) ? (int)TheGameLogic->getFrame() : -1;
			int objs = TheGameLogic ? (int)TheGameLogic->getObjectCount() : -1;
			fprintf(stderr, "[PERF warn] frame avg=%.2fms max=%.2fms logic=%.2fms /%d frames gameFrame=%d objects=%d\n",
				pf_acc / pf_n, pf_max, g_wasmStressLogicMs, pf_n, gf, objs);
			pf_n = 0; pf_acc = 0; pf_max = 0;
		}
	}
}
#endif

void GameEngine::execute()
{
#if defined(RTS_DEBUG)
	DWORD startTime = timeGetTime() / 1000;
#endif

#ifdef __EMSCRIPTEN__
	// Non-blocking browser main loop (0 = requestAnimationFrame cadence, 1 =
	// simulate infinite loop so this call unwinds and keeps the runtime alive).
	emscripten_set_main_loop(wasm_engine_frame, 0, 1);
	return;
#endif

	// pretty basic for now
	while( !m_quitting )
	{

		//if (TheGlobalData->m_vTune)
		{
#ifdef PERF_TIMERS
			PerfGather::resetAll();
#endif
		}

		{

#if defined(RTS_DEBUG)
			{
				// enter only if in benchmark mode
				if (TheGlobalData->m_benchmarkTimer > 0)
				{
					DWORD currentTime = timeGetTime() / 1000;
					if (TheGlobalData->m_benchmarkTimer < currentTime - startTime)
					{
						if (TheGameLogic->isInGame())
						{
							if (TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
							{
								TheRecorder->stopRecording();
							}
							TheGameLogic->clearGameData();
						}
						TheGameEngine->setQuitting(TRUE);
					}
				}
			}
#endif

			{
				try
				{
					// compute a frame
					update();
				}
				catch (INIException e)
				{
					// Release CRASH doesn't return, so don't worry about executing additional code.
					if (e.mFailureMessage)
						RELEASE_CRASH((e.mFailureMessage));
					else
						RELEASE_CRASH(("Uncaught Exception in GameEngine::update"));
				}
				catch (...)
				{
					// try to save info off
					try
					{
						if (TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_RECORD && TheRecorder->isMultiplayer())
							TheRecorder->cleanUpReplayFile();
					}
					catch (...)
					{
					}
					RELEASE_CRASH(("Uncaught Exception in GameEngine::update"));
				}
			}

			TheFramePacer->update();

			// NOTE: TheDisplay->draw() is called via TheGameClient->UPDATE() above.
			// GameClient::update() dispatches TheDisplay->DRAW() each frame.
			// Do NOT add an extra draw() call here - it would double-present per frame.
		}

#ifdef PERF_TIMERS
		if (!m_quitting && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame() && !TheGameLogic->isGamePaused())
		{
			PerfGather::dumpAll(TheGameLogic->getFrame());
			PerfGather::displayGraph(TheGameLogic->getFrame());
			PerfGather::resetAll();
		}
#endif

	}
}

/** -----------------------------------------------------------------------------------------------
	* Factory for the message stream
	*/
MessageStream *GameEngine::createMessageStream()
{
	// if you change this update the tools that use the engine systems
	// like GUIEdit, it creates a message stream to run in "test" mode
	return MSGNEW("GameEngineSubsystem") MessageStream;
}

//-------------------------------------------------------------------------------------------------
FileSystem *GameEngine::createFileSystem()
{
	return MSGNEW("GameEngineSubsystem") FileSystem;
}

//-------------------------------------------------------------------------------------------------
Bool GameEngine::isMultiplayerSession()
{
	return TheRecorder->isMultiplayer();
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
#define CONVERT_EXEC1	"..\\Build\\nvdxt -list buildDDS.txt -dxt5 -full -outdir Art\\Textures > buildDDS.out"

void updateTGAtoDDS()
{
	// Here's the scoop. We're going to traverse through all of the files in the Art\Textures folder
	// and determine if there are any .tga files that are newer than associated .dds files. If there
	// are, then we will re-run the compression tool on them.

	File *fp = TheLocalFileSystem->openFile("buildDDS.txt", File::WRITE | File::CREATE | File::TRUNCATE | File::TEXT);
	if (!fp) {
		return;
	}

	FilenameList files;
	TheLocalFileSystem->getFileListInDirectory("Art\\Textures\\", "", "*.tga", files, TRUE);
	FilenameList::iterator it;
	for (it = files.begin(); it != files.end(); ++it) {
		AsciiString filenameTGA = *it;
		AsciiString filenameDDS = *it;
		FileInfo infoTGA;
		TheLocalFileSystem->getFileInfo(filenameTGA, &infoTGA);

		// skip the water textures, since they need to be NOT compressed
		filenameTGA.toLower();
		if (strstr(filenameTGA.str(), "caust"))
		{
			continue;
		}
		// and the recolored stuff.
		if (strstr(filenameTGA.str(), "zhca"))
		{
			continue;
		}

		// replace tga with dds
		filenameDDS.truncateBy(3); // tga
		filenameDDS.concat("dds");

		Bool needsToBeUpdated = FALSE;
		FileInfo infoDDS;
		if (TheFileSystem->doesFileExist(filenameDDS.str())) {
			TheFileSystem->getFileInfo(filenameDDS, &infoDDS);
			if (infoTGA.timestampHigh > infoDDS.timestampHigh ||
					(infoTGA.timestampHigh == infoDDS.timestampHigh &&
					 infoTGA.timestampLow > infoDDS.timestampLow)) {
				needsToBeUpdated = TRUE;
			}
		} else {
			needsToBeUpdated = TRUE;
		}

		if (!needsToBeUpdated) {
			continue;
		}

		filenameTGA.concat("\n");
		fp->write(filenameTGA.str(), filenameTGA.getLength());
	}

	fp->close();

// TheSuperHackers @build fighter19 11/02/2026 Windows-only texture conversion
#ifdef _WIN32
	system(CONVERT_EXEC1);
#else
	// Linux: TGA to DDS conversion not needed (or handle differently)
#endif
}

//-------------------------------------------------------------------------------------------------
// System things

// If we're using the Wide character version of MessageBox, then there's no additional
// processing necessary. Please note that this is a sleazy way to get this information,
// but pending a better one, this'll have to do.
// TheSuperHackers @build fighter19 11/02/2026 MessageBox detection (Windows-only)
#ifdef _WIN32
extern const Bool TheSystemIsUnicode = (((void*) (::MessageBox)) == ((void*) (::MessageBoxW)));
#else
extern const Bool TheSystemIsUnicode = true;  // Linux: Always Unicode (UTF-8)
#endif
