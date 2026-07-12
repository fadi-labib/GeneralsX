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
add_link_options(-pthread -sPROXY_TO_PTHREAD=1 -sALLOW_MEMORY_GROWTH=1)

# WebGL2/GLES3: dx8wasm renders through a WebGL2 context. Match its smoke build.
# OFFSCREENCANVAS_SUPPORT: main() runs on the PROXY_TO_PTHREAD worker, so the GL
# context is created off the main thread - which requires an OffscreenCanvas.
add_link_options(-sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sFULL_ES3=1
  -sOFFSCREENCANVAS_SUPPORT=1 "-sOFFSCREENCANVASES_TO_PTHREAD=#canvas")

# FORCE_FILESYSTEM: game data (.big archives) is delivered as a separate
# file_packager bundle loaded at runtime, so the engine must include FS support.
add_link_options(-sFORCE_FILESYSTEM=1)

# Feature flags the engine's option tree keys off. SDL3 comes from the
# emscripten port at link time; audio deferred; math helpers via GLM/GLI.
set(SAGE_USE_SDL3 ON  CACHE BOOL "" FORCE)
set(SAGE_USE_GLM  ON  CACHE BOOL "" FORCE)
# Audio backend: MiniAudio (single-header) rather than OpenAL. The OpenAL device
# hard-requires FFmpeg (libavcodec) for decoding, which is heavy to build for wasm;
# MiniAudio replaces OpenAL+FFmpeg for audio with no extra deps and still supplies
# the -DSAGE_USE_MINIAUDIO that activates MilesStub. Sound output is a later plan.
set(SAGE_USE_OPENAL    OFF CACHE BOOL "" FORCE)
set(SAGE_USE_MINIAUDIO ON  CACHE BOOL "" FORCE)
set(SAGE_UPDATE_CHECK OFF CACHE BOOL "" FORCE)  # libcurl update-checker: networking, later plan
# Deterministic (fdlibm) math is for cross-platform replay/MP bit-exactness - a
# later plan. Its fetched sources assume x86 fenv (FE_INVALID/FE_INEXACT) that
# emscripten's fenv.h lacks. Engine has a CRT-math fallback when this is OFF.
set(SAGE_USE_DETERMINISTIC_MATH OFF CACHE BOOL "" FORCE)
# Crash-dump writing is Windows-only (MiniDumpWriteDump / FILETIME). Off on wasm.
set(RTS_CRASHDUMP_ENABLE OFF CACHE BOOL "" FORCE)
# SagePatch is a Linux LD_PRELOAD interposer (.so) - meaningless on wasm.
set(RTS_BUILD_OPTION_SAGE_PATCH OFF CACHE BOOL "" FORCE)
