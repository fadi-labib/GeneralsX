# GeneralsX @build dx8wasm - Emscripten (WASM) enablement. Guarded: only active
# when CMAKE_SYSTEM_NAME == Emscripten. Desktop/macOS/Linux never see this file.
if(NOT EMSCRIPTEN)
  return()
endif()

message(STATUS "GeneralsX: Emscripten build enabled (dx8wasm backend)")

# Header-only deps the engine expects from vcpkg -> FetchContent on wasm.
include(FetchContent)
# OVERRIDE_FIND_PACKAGE: the engine calls find_package(glm/gli CONFIG REQUIRED)
# in several CMakeLists; this redirects those to the fetched copies (no install).
FetchContent_Declare(glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.1
  OVERRIDE_FIND_PACKAGE)
FetchContent_Declare(gli
  GIT_REPOSITORY https://github.com/g-truc/gli.git
  GIT_TAG 779b99ac6656e4d30c3b24e96e0136a59649a869  # gli has no release tags; pinned SHA
  OVERRIDE_FIND_PACKAGE)
FetchContent_MakeAvailable(glm gli)

# zlib + freetype via Emscripten ports (compile + link flags).
add_compile_options(-sUSE_ZLIB=1 -sUSE_FREETYPE=1)
add_link_options(-sUSE_ZLIB=1 -sUSE_FREETYPE=1)

# gli's reduce.inl misuses `.template fetch(` on a non-template member; newer
# Clang (emscripten) promotes this to a hard error. gli is fetched (can't patch
# cleanly), so demote the diagnostic. Harmless: the construct is valid pre-C++20.
add_compile_options(-Wno-missing-template-arg-list-after-template-kw)

# Engine boots off the main thread (blocking init + sync file I/O).
add_compile_options(-pthread)
# Fixed (non-growable) memory: with pthreads + WebGL2, growable shared memory is
# exposed as a *resizable* ArrayBuffer, and WebGL rejects typed-array views of it
# ("must not be resizable") - which breaks glUniformMatrix4fv etc. A large fixed
# heap keeps the SharedArrayBuffer non-resizable so WebGL accepts the views.
add_link_options(-pthread -sPROXY_TO_PTHREAD=1
  -sINITIAL_MEMORY=3758096384 -sALLOW_MEMORY_GROWTH=0)  # 3.5 GB fixed (holds the in-game asset bundle
  # incl. full audio ~2GB + engine runtime; wasm32 caps at 4GB. Non-growable so WebGL accepts typed-
  # array views of the SharedArrayBuffer. Raised from 2GB when base-game audio was added (~640MB).

# Exceptions: the engine uses try/catch for control flow (INI loader throws
# INI_CANT_OPEN_FILE etc., catches, sometimes re-throws). Emscripten disables
# exception catching by default so those catches are dead and throws abort.
# Use -fwasm-exceptions (native WASM EH) not -fexceptions (legacy JS-based EH):
# the JS-based unwinder hangs under PROXY_TO_PTHREAD when an exception is
# re-thrown across the worker. Native WASM EH works with pthreads and is faster.
add_compile_options(-fwasm-exceptions)
add_link_options(-fwasm-exceptions)

# WebGL2/GLES3: dx8wasm renders through a WebGL2 context. main() runs on the
# PROXY_TO_PTHREAD worker, so the canvas is transferred to that worker
# (OffscreenCanvas). The worker owns the canvas + its requestAnimationFrame, which
# drives GameEngine::execute's emscripten_set_main_loop; each rAF renders one frame
# and the OffscreenCanvas auto-presents it to the page.
add_link_options(-sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sFULL_ES3=1
  -sOFFSCREENCANVAS_SUPPORT=1 "-sOFFSCREENCANVASES_TO_PTHREAD=#canvas")

# FORCE_FILESYSTEM: game data (.big archives) is delivered as a separate
# file_packager bundle loaded at runtime, so the engine must include FS support.
add_link_options(-sFORCE_FILESYSTEM=1)

# IDBFS: player-owned data (Save/, Replays/, options.ini, user maps) lives under /gxuser,
# which web/user-data.js mounts as IndexedDB-backed storage so it survives a tab reload.
# Without -lidbfs.js the IDBFS symbol does not exist and everything stays in RAM (MEMFS).
# See GlobalData::BuildUserDataPathFromRegistry's __EMSCRIPTEN__ branch.
add_link_options(-lidbfs.js)

# Export FS to JS. Emscripten strips unreferenced runtime objects, so without this
# `Module.FS` is a stub that aborts with "'FS' was not exported" the moment page-side code
# touches it - which broke BOTH web/user-data.js (the IDBFS mount) and web/byo-assets.js
# (the bring-your-own-assets import, whose whole job is FS.writeFile). Caught by
# web-runtime/user-data-test.mjs; the BYO path had the same latent failure.
add_link_options(-sEXPORTED_RUNTIME_METHODS=FS)

# Embed a TrueType font at fonts/arial.ttf in MEMFS. The web has no fontconfig, so
# FontCharsClass::Locate_Font_FontConfig (render2dsentence.cpp) resolves fonts from
# this bundled path (arial.ttf = universal fallback; the UI is Arial-based). Without
# it FreeType loads no face and NO in-game text renders. LiberationSans is a
# metric-compatible, freely-licensed Arial substitute.
add_link_options("SHELL:--embed-file ${CMAKE_CURRENT_SOURCE_DIR}/Data/Fonts/arial.ttf@fonts/arial.ttf")

# Feature flags the engine's option tree keys off. SDL3 comes from the
# emscripten port at link time; audio deferred; math helpers via GLM/GLI.
set(SAGE_USE_SDL3 ON  CACHE BOOL "" FORCE)
set(SAGE_USE_GLM  ON  CACHE BOOL "" FORCE)
# Audio backend: OpenAL via Emscripten's built-in -lopenal (Web Audio backend), which
# works under PROXY_TO_PTHREAD. MiniAudio's WebAudio backend needs ASYNCIFY/AUDIO_WORKLET
# (not enabled here) so it inits a context but outputs no sound on wasm. OpenAL decodes
# through the FFmpeg built below (RTS_BUILD_OPTION_FFMPEG). Same choice as the WASM +
# Android reference forks. (MiniAudio path kept in-tree but disabled on wasm.)
set(SAGE_USE_OPENAL    ON  CACHE BOOL "" FORCE)
set(SAGE_USE_MINIAUDIO OFF CACHE BOOL "" FORCE)
add_link_options(-lopenal)
# Audio DECODE + Bink video need libavcodec. There's no vcpkg FFmpeg for wasm32, so
# GameEngineDevice/CMakeLists builds a minimal one from source (cmake/ffmpeg-emscripten.cmake,
# WAV/MP3/Bink only). Turning this ON un-stubs MiniAudioManager's decode path (which otherwise
# leaves PCM empty on wasm = silence) and compiles the existing FFmpegVideoPlayer.
set(RTS_BUILD_OPTION_FFMPEG ON CACHE BOOL "" FORCE)
set(SAGE_UPDATE_CHECK OFF CACHE BOOL "" FORCE)  # libcurl update-checker: networking, later plan
# Deterministic (fdlibm) math is for cross-platform replay/MP bit-exactness - a
# later plan. Its fetched sources assume x86 fenv (FE_INVALID/FE_INEXACT) that
# emscripten's fenv.h lacks. Engine has a CRT-math fallback when this is OFF.
set(SAGE_USE_DETERMINISTIC_MATH OFF CACHE BOOL "" FORCE)
# Crash-dump writing is Windows-only (MiniDumpWriteDump / FILETIME). Off on wasm.
set(RTS_CRASHDUMP_ENABLE OFF CACHE BOOL "" FORCE)
# SagePatch is a Linux LD_PRELOAD interposer (.so) - meaningless on wasm.
set(RTS_BUILD_OPTION_SAGE_PATCH OFF CACHE BOOL "" FORCE)
