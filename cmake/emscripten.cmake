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
#
# GX_HEAP_MB overrides the size. It must stay a FIXED size whatever you choose — do not switch
# to ALLOW_MEMORY_GROWTH to save memory, for the reason above.
#
# The heap does NOT have to exceed the asset bundle. This comment used to say it did, and that
# was wrong — it cost the project a documented "mobile is blocked by the fixed heap" conclusion
# that was never true. Emscripten's file_packager passes canOwn=TRUE to FS_createDataFile, and
# MEMFS then stores `node.contents = buffer.subarray(...)` — a REFERENCE into the ArrayBuffer the
# fetch produced. That buffer lives in the JS heap, NOT in wasm memory, and read-only .big
# archives are never written to, so they are never copied in.
#
# MEASURED 2026-08-04 against the 1295.64 MiB packed bundle (build/shrunk-audio-only): a Release
# engine at GX_HEAP_MB=1024, 512 and even 256 all boot Alpine Assault to a full 60 FPS match —
# terrain, HUD, radar, units — reaching simulation frame ~8780-8822 with zero aborts and no OOM.
# 256 MiB is a FIFTH of the bundle. So the number below buys nothing for asset delivery.
#
# What actually costs memory, and where mobile is really blocked: the fetched bundle occupies
# ~1× its size in the JS heap for the whole session, and TRANSIENTLY ~2× during download, because
# file_packager's fetchRemotePackage() accumulates every chunk in an array and then allocates a
# second full-size Uint8Array to concatenate them. For a 1.27 GiB bundle that is a ~2.6 GiB peak
# no heap setting can influence. The fix is page-side and supported: implement
# Module.getPreloadedPackage (file_packager skips its own fetch when it exists) and stream into
# one preallocated buffer, since REMOTE_PACKAGE_SIZE/Content-Length gives the size up front.
#
# So: keep this large on desktop because it is free there, but do not treat it as the mobile
# blocker. Measure a candidate with:
#   emcmake cmake ... -DGX_HEAP_MB=1024      # link-only change, ~15 s to re-link
set(GX_HEAP_MB "3584" CACHE STRING "Fixed wasm heap size in MB (fixed, non-growable; need NOT exceed the asset bundle)")
math(EXPR GX_HEAP_BYTES "${GX_HEAP_MB} * 1024 * 1024")
message(STATUS "GeneralsX: fixed wasm heap = ${GX_HEAP_MB} MB")
add_link_options(-pthread -sPROXY_TO_PTHREAD=1
  -sINITIAL_MEMORY=${GX_HEAP_BYTES} -sALLOW_MEMORY_GROWTH=0)  # engine runtime only — the asset
  # bundle is NOT in here (see above: MEMFS references the JS-side fetch buffer). wasm32 caps at
  # 4GB. Non-growable so WebGL accepts typed-array views of the SharedArrayBuffer. It was raised to
  # 3.5GB on the belief that the bundle had to fit; that belief was disproved 2026-08-04, and it is
  # kept only because headroom is free on desktop.

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
#
# HEAPU8 and wasmMemory are exported for the same class of reason, added with the OPFS work:
# the page has to hand the I/O worker the wasm memory object itself (it is the SharedArrayBuffer
# the control block lives in), and Emscripten attaches neither to Module unless asked. Without
# them byo-assets.js throws "Cannot read properties of undefined (reading 'buffer')" and falls
# back to the in-RAM mount — which boots perfectly, so the loss is invisible without a harness.
add_link_options(-sEXPORTED_RUNTIME_METHODS=FS,HEAPU8,wasmMemory)

# GeneralsX @build dx8wasm - on-demand OPFS archive reads (web/byo-assets.js with ?opfs=1).
# The page must allocate the shared control block and learn its address before it can start the
# I/O worker, so these two dx8wasm entry points have to survive dead-code elimination. If they
# are missing the page cannot enable the feature and silently keeps the in-RAM mount, which is
# exactly the failure that looks like success.
add_link_options(-sEXPORTED_FUNCTIONS=_main,_dx8wasm_opfs_init,_dx8wasm_opfs_control_addr)

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
