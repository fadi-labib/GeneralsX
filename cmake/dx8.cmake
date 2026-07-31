# DirectX 8 headers and rendering backend selection
# GeneralsX @build BenderAI 10/02/2026 - Session 18
# Fighter19's approach: Fetch ONE OR THE OTHER, never both
#
# On Windows: Use min-dx8-sdk (minimal Windows DirectX headers + libs)
# On Linux:   Use DXVK native pre-built tarball (DirectX→Vulkan translation)
# On macOS:   Build DXVK from source using Meson + MoltenVK (DirectX→Metal bridge)
#
# CRITICAL: Mixing headers causes conflicts - dx8-src has incomplete types,
# DXVK has full DirectX8+Wine headers. Compiler picks first path = wrong headers.
#
# macOS DXVK build (Session 61, 24/02/2026):
#   DXVK 2.6 builds natively on macOS arm64 via its "native" build mode.
#   macOS fixes are maintained in the DXVK fork history consumed by this build.
#   This project no longer applies local patch scripts during configure/build.
#
# Reference: docs/WORKDIR/lessons/2026-02-LESSONS.md (historical patch rationale)

set(DXVK_VERSION "v2.6")

if(SAGE_USE_DX8)
  # Windows: Fetch min-dx8-sdk for native DirectX 8
  FetchContent_Declare(
    dx8
    GIT_REPOSITORY https://github.com/TheSuperHackers/min-dx8-sdk.git
    GIT_TAG        7bddff8c01f5fb931c3cb73d4aa8e66d303d97bc
  )
  FetchContent_MakeAvailable(dx8)
  message(STATUS "Using DirectX 8 SDK (Windows native)")

elseif(APPLE AND SAGE_USE_MOLTENVK)
  # macOS: Build DXVK 2.6 from source using Meson + MoltenVK
  # GeneralsX @build BenderAI 24/02/2026 - Phase 5 macOS port (Session 61)
  find_program(MESON_EXECUTABLE meson HINTS /usr/local/bin /opt/homebrew/bin)
  find_program(NINJA_EXECUTABLE ninja HINTS /usr/local/bin /opt/homebrew/bin)

  if(NOT MESON_EXECUTABLE)
    message(FATAL_ERROR "DXVK macOS build requires meson: brew install meson")
  endif()
  if(NOT NINJA_EXECUTABLE)
    message(FATAL_ERROR "DXVK macOS build requires ninja: brew install ninja")
  endif()

  # Detect host architecture so Clang targets the correct slice.
  # IMPORTANT: prefer CMAKE_OSX_ARCHITECTURES (set by the preset) over uname -m.
  # On Apple Silicon Macs running CMake / meson via Rosetta, uname -m returns
  # x86_64 even though the native executable arch is arm64. Using CMAKE_OSX_ARCHITECTURES
  # (e.g. "arm64" from the macos-vulkan preset) avoids building an x86_64 dylib that
  # the arm64 game binary cannot dlopen.
  if(CMAKE_OSX_ARCHITECTURES)
    # Use the first entry (handles "arm64;x86_64" fat-binary requests too)
    list(GET CMAKE_OSX_ARCHITECTURES 0 DXVK_HOST_ARCH)
  else()
    execute_process(
      COMMAND uname -m
      OUTPUT_VARIABLE DXVK_HOST_ARCH
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
  endif()
  message(STATUS "Building DXVK ${DXVK_VERSION} for macOS/${DXVK_HOST_ARCH} with Meson (${MESON_EXECUTABLE})")

  include(ExternalProject)
  # GeneralsX @build BenderAI 13/03/2026 Add explicit source mode to keep remote branch updates deterministic by default.
  set(DXVK_LOCAL_FORK_DIR "${CMAKE_SOURCE_DIR}/references/fbraz3-dxvk")
  option(SAGE_DXVK_USE_LOCAL_FORK "Build DXVK from local references/fbraz3-dxvk checkout" OFF)

  if(SAGE_DXVK_USE_LOCAL_FORK AND EXISTS "${DXVK_LOCAL_FORK_DIR}/.git")
    set(DXVK_SOURCE_DIR "${DXVK_LOCAL_FORK_DIR}")
    message(STATUS "DXVK macOS build: using local fork source at ${DXVK_SOURCE_DIR}")
  else()
    set(DXVK_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/dxvk-src-fbraz3")
    message(STATUS "DXVK macOS build: using GitHub source clone at ${DXVK_SOURCE_DIR}")
  endif()
  set(DXVK_BUILD_DIR  "${CMAKE_BINARY_DIR}/_deps/dxvk-build-macos")
  set(DXVK_D3D8_LIB  "${DXVK_BUILD_DIR}/src/d3d8/libdxvk_d3d8.0.dylib")
  set(DXVK_D3D9_LIB  "${DXVK_BUILD_DIR}/src/d3d9/libdxvk_d3d9.0.dylib")

  # Detect Vulkan SDK location for Meson configuration.
  # VULKAN_SDK must point to the platform subdir (e.g. ~/VulkanSDK/1.4.x/macOS)
  # where lib/libvulkan.dylib and lib/libMoltenVK.dylib live.
  # GeneralsX @build BenderAI 03/03/2026: Normalize env path to macOS platform subdir
  set(VULKAN_SDK_ENV "$ENV{VULKAN_SDK}")

  # If VULKAN_SDK points to the version root (has macOS/ subdir), normalize it
  if(VULKAN_SDK_ENV AND EXISTS "${VULKAN_SDK_ENV}/macOS/lib/libMoltenVK.dylib")
    set(VULKAN_SDK_ENV "${VULKAN_SDK_ENV}/macOS")
    message(STATUS "DXVK macOS build: Normalized VULKAN_SDK to platform subdir: ${VULKAN_SDK_ENV}")
  endif()

  if(NOT VULKAN_SDK_ENV OR NOT EXISTS "${VULKAN_SDK_ENV}/lib/libMoltenVK.dylib")
    # Try home directory: look for ~/VulkanSDK/*/macOS
    file(GLOB VULKAN_HOME_DIRS "$ENV{HOME}/VulkanSDK/*/macOS")
    if(VULKAN_HOME_DIRS)
      list(SORT VULKAN_HOME_DIRS)
      list(REVERSE VULKAN_HOME_DIRS)
      list(GET VULKAN_HOME_DIRS 0 POTENTIAL_SDK)
      if(EXISTS "${POTENTIAL_SDK}/lib/libMoltenVK.dylib")
        set(VULKAN_SDK_ENV "${POTENTIAL_SDK}")
      endif()
    endif()
  endif()

  if(NOT VULKAN_SDK_ENV OR NOT EXISTS "${VULKAN_SDK_ENV}/lib/libMoltenVK.dylib")
    # Try common Homebrew locations
    foreach(BREW_PATH "/usr/local/Caskroom/vulkan-sdk/latest/VulkanSDK/macOS" "/opt/homebrew/Caskroom/vulkan-sdk/latest/VulkanSDK/macOS")
      if(EXISTS "${BREW_PATH}/lib/libMoltenVK.dylib")
        set(VULKAN_SDK_ENV "${BREW_PATH}")
        break()
      endif()
    endforeach()
  endif()

  if(VULKAN_SDK_ENV AND EXISTS "${VULKAN_SDK_ENV}/lib/libMoltenVK.dylib")
    message(STATUS "DXVK macOS build: Using Vulkan SDK at ${VULKAN_SDK_ENV}")
    set(VULKAN_SDK_ENV_VAR "VULKAN_SDK=${VULKAN_SDK_ENV}")
  else()
    message(WARNING "DXVK macOS build: Vulkan SDK / MoltenVK not found; Meson will search system paths")
    if(VULKAN_SDK_ENV)
      message(STATUS "  VULKAN_SDK checked: ${VULKAN_SDK_ENV}")
    endif()
    set(VULKAN_SDK_ENV_VAR "")
  endif()

  if(SAGE_DXVK_USE_LOCAL_FORK AND EXISTS "${DXVK_LOCAL_FORK_DIR}/.git")
    ExternalProject_Add(dxvk_macos_build
      # GeneralsX @build BenderAI 13/03/2026 Build from local fbraz3 fork to avoid stale remote hash pins.
      SOURCE_DIR        ${DXVK_SOURCE_DIR}
      BINARY_DIR        ${DXVK_BUILD_DIR}
      DOWNLOAD_COMMAND  ""
      UPDATE_COMMAND    ""
      PATCH_COMMAND     ""
      CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env CC=clang CXX=clang++ "CFLAGS=-arch ${DXVK_HOST_ARCH} -mcpu=apple-m1" "CXXFLAGS=-arch ${DXVK_HOST_ARCH} -mcpu=apple-m1" "LDFLAGS=-arch ${DXVK_HOST_ARCH}" ${VULKAN_SDK_ENV_VAR} ${MESON_EXECUTABLE} setup ${DXVK_BUILD_DIR} ${DXVK_SOURCE_DIR} --native-file ${CMAKE_SOURCE_DIR}/cmake/meson-arm64-native.ini -Ddxvk_native_wsi=sdl3 --buildtype=release --reconfigure
      BUILD_COMMAND     ${NINJA_EXECUTABLE} -C ${DXVK_BUILD_DIR} src/d3d9/libdxvk_d3d9.0.dylib src/d3d8/libdxvk_d3d8.0.dylib
      INSTALL_COMMAND   ""
      UPDATE_DISCONNECTED TRUE
    )
  else()
    # GeneralsX @build copilot 01/04/2026 Pin remote DXVK to immutable commit produced by fix/macos-size_t-cstddef.
    set(DXVK_REMOTE_REF 46a3bc018bcae408d49d3c500e4e536a11f6789a)
    ExternalProject_Add(dxvk_macos_build
      # GeneralsX @build BenderAI 08/04/2026 Consume pre-patched source from pinned fork commit.
      GIT_REPOSITORY    https://github.com/fbraz3/dxvk.git
      GIT_TAG           ${DXVK_REMOTE_REF}
      # GeneralsX @build copilot 01/04/2026 Keep pinned commit fetch reliable across clean CI builds.
      GIT_SHALLOW       FALSE
      SOURCE_DIR        ${DXVK_SOURCE_DIR}
      BINARY_DIR        ${DXVK_BUILD_DIR}
      PATCH_COMMAND     ""
      CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env CC=clang CXX=clang++ "CFLAGS=-arch ${DXVK_HOST_ARCH} -mcpu=apple-m1" "CXXFLAGS=-arch ${DXVK_HOST_ARCH} -mcpu=apple-m1" "LDFLAGS=-arch ${DXVK_HOST_ARCH}" ${VULKAN_SDK_ENV_VAR} ${MESON_EXECUTABLE} setup ${DXVK_BUILD_DIR} ${DXVK_SOURCE_DIR} --native-file ${CMAKE_SOURCE_DIR}/cmake/meson-arm64-native.ini -Ddxvk_native_wsi=sdl3 --buildtype=release --reconfigure
      BUILD_COMMAND     ${NINJA_EXECUTABLE} -C ${DXVK_BUILD_DIR} src/d3d9/libdxvk_d3d9.0.dylib src/d3d8/libdxvk_d3d8.0.dylib
      INSTALL_COMMAND   ""
      UPDATE_DISCONNECTED FALSE
    )
  endif()

  # Copy libdxvk_d3d9 + libdxvk_d3d8 to build dir and create unversioned symlinks.
  # d3d8 links against d3d9 via @rpath, so both must be present at runtime.
  add_custom_command(
    OUTPUT  "${CMAKE_BINARY_DIR}/libdxvk_d3d9.0.dylib"
            "${CMAKE_BINARY_DIR}/libdxvk_d3d8.0.dylib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${DXVK_D3D9_LIB} "${CMAKE_BINARY_DIR}/libdxvk_d3d9.0.dylib"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
              libdxvk_d3d9.0.dylib "${CMAKE_BINARY_DIR}/libdxvk_d3d9.dylib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${DXVK_D3D8_LIB} "${CMAKE_BINARY_DIR}/libdxvk_d3d8.0.dylib"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
              libdxvk_d3d8.0.dylib "${CMAKE_BINARY_DIR}/libdxvk_d3d8.dylib"
    DEPENDS dxvk_macos_build
    COMMENT "Installing libdxvk_d3d8 + libdxvk_d3d9 to build directory"
  )
  add_custom_target(dxvk_d3d8_install ALL
    DEPENDS "${CMAKE_BINARY_DIR}/libdxvk_d3d8.0.dylib"
            "${CMAKE_BINARY_DIR}/libdxvk_d3d9.0.dylib"
  )

  # Export path so other cmake files know where the headers are
  set(DXVK_INCLUDE_DIR "${DXVK_SOURCE_DIR}/include/native" CACHE PATH "DXVK native headers")
  # GeneralsX @build felipebraz 10/06/2025 Mirror lowercase dxvk_SOURCE_DIR that FetchContent sets on Linux
  # so CompatLib/CMakeLists.txt check works on macOS as well (CACHE PATH survives auto-regeneration)
  set(dxvk_SOURCE_DIR "${DXVK_SOURCE_DIR}" CACHE PATH "DXVK source directory (macOS)")
  message(STATUS "DXVK source directory: ${DXVK_SOURCE_DIR}")
  message(STATUS "DXVK d3d8 library:     ${DXVK_D3D8_LIB}")

else()
  # Linux: Fetch pre-built DXVK native binary for DirectX→Vulkan translation
  # Native 32-bit and 64-bit Linux binaries (.so)
  FetchContent_Declare(
    dxvk
    URL        https://github.com/doitsujin/dxvk/releases/download/v2.6/dxvk-native-2.6-steamrt-sniper.tar.gz
  )
  FetchContent_MakeAvailable(dxvk)
  message(STATUS "Using DXVK native (Linux DirectX→Vulkan)")
  message(STATUS "DXVK source directory: ${dxvk_SOURCE_DIR}")
endif()

# GeneralsX @build dx8wasm - Emscripten backend.
# The engine keeps DXVK's native d3d8.h (fetched in the else() branch above) as
# its ABI; dx8wasm provides the *implementation* (Direct3DCreate8 + the full COM
# vtable), proven ABI-compatible with that header in M2. We compile dx8wasm's
# backend sources directly into a static lib (not add_subdirectory, which would
# drag in dx8wasm's SDL3-port smoke-test executables and clash with the engine's
# own SDL3). The GL/context/platform layer is deferred to the runtime plan.
if(EMSCRIPTEN)
  if(NOT DEFINED DX8WASM_DIR)
    message(FATAL_ERROR "DX8WASM_DIR not set (point at the dx8wasm sibling repo)")
  endif()
  # Platform seam (M5 context reconciliation): dx8wasm's device.cpp calls
  # platform::{create,present,destroy,alive}_gl_context. On wasm the ENGINE owns
  # the window + GL context (SDL3Main.cpp creates an SDL_WINDOW_OPENGL ES3
  # context); dx8wasm ADOPTS the current context instead of creating its own
  # window. SDL3 has C linkage, so we forward-declare the few calls we need and
  # let them resolve at link (the app links SDL3) - no SDL headers required here.
  set(_dx8_stub ${CMAKE_BINARY_DIR}/dx8wasm_platform_seam.cpp)
  file(WRITE ${_dx8_stub}
"// Generated by cmake/dx8.cmake - dx8wasm platform seam for wasm.\n"
"// Engine owns the SDL window+context; dx8wasm adopts the current one.\n"
"extern \"C\" {\n"
"  typedef void* SDL_GLContext;\n"
"  struct SDL_Window;\n"
"  SDL_GLContext SDL_GL_GetCurrentContext(void);\n"
"  SDL_Window* SDL_GL_GetCurrentWindow(void);\n"
"  bool SDL_GL_SwapWindow(SDL_Window*);\n"
"}\n"
"#include <emscripten.h>\n"
"#include <emscripten/html5_webgl.h>\n"
"#include <dx8wasm/telemetry.h>\n"
"namespace platform {\n"
"bool create_gl_context(int, int) { return SDL_GL_GetCurrentContext() != nullptr; }\n"
"// A browser can revoke the WebGL context at any time, and mobile browsers routinely do it\n"
"// once a tab has been backgrounded - the same hazard the iOS/Android ports handle by pausing\n"
"// around DID_ENTER_BACKGROUND (there, the drawable is gone). Afterwards every GL call is a\n"
"// silent no-op that just sets an error, so the game keeps running and keeps 'rendering' into\n"
"// nothing: a black canvas with no diagnostic, which a player reads as a crash.\n"
"// We cannot recover transparently - that would mean re-uploading every texture, buffer and\n"
"// shader, and the engine has no device-reset path here. So detect it, stop touching the GPU,\n"
"// and tell the page once so it can say something actionable. present() is the natural place:\n"
"// every frame passes through it, on the thread that owns the context.\n"
"void present() {\n"
"  static bool s_lost = false;\n"
"  if (!s_lost) {\n"
"    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE c = emscripten_webgl_get_current_context();\n"
"    if (c && emscripten_is_webgl_context_lost(c)) {\n"
"      s_lost = true;\n"
"      MAIN_THREAD_EM_ASM({\n"
"        console.error('[gl] WebGL context lost - rendering stopped');\n"
"        if (typeof gxContextLost === 'function') gxContextLost();\n"
"      });\n"
"    }\n"
"  }\n"
"  // Telemetry rides the frame boundary: present() is the one function that runs every\n"
"  // frame on the thread owning the GL context. The pump self-rate-limits to 1 Hz.\n"
"  // It must drain BEFORE the context-loss bail-out below: it touches no GPU state, and\n"
"  // leaving it after the return kills telemetry at exactly the incident whose records matter\n"
"  // most (gxContextLost), losing whatever is still queued in the ring at the moment of loss.\n"
"  dx8wasm_tel_pump();\n"
"  if (s_lost) return;\n"
"  SDL_Window* w = SDL_GL_GetCurrentWindow(); if (w) SDL_GL_SwapWindow(w);\n"
"}\n"
"void destroy_gl_context() {}\n"
"bool gl_context_alive() { return SDL_GL_GetCurrentContext() != nullptr; }\n"
"}\n"
"\n"
"// libc++ atomic-sync backend: emscripten's -fno-exceptions (noexcept) libc++\n"
"// variant omits std::__libcpp_atomic_wait, which the engine's std::atomic::wait\n"
"// pulls in. Provide a no-op (busy-poll semantics; the caller re-checks the\n"
"// predicate). Threading correctness revisited when it matters.\n"
"#include <cstdint>\n"
"namespace std { inline namespace __2 {\n"
"void __libcpp_atomic_wait(void const volatile*, int32_t) {}\n"
"}}\n")
  add_library(dx8wasm_backend STATIC
    ${DX8WASM_DIR}/runtime/d3d8webgl/d3d8.cpp
    ${DX8WASM_DIR}/runtime/d3d8webgl/device.cpp
    ${DX8WASM_DIR}/runtime/graphics-ff/ff_shader.cpp
    ${DX8WASM_DIR}/runtime/coverage/coverage.cpp
    ${DX8WASM_DIR}/runtime/telemetry/telemetry.cpp
    ${DX8WASM_DIR}/runtime/runtime.cpp
    ${_dx8_stub})
  target_include_directories(dx8wasm_backend PUBLIC
    ${DX8WASM_DIR}/runtime ${DX8WASM_DIR}/runtime/d3d8 ${DX8WASM_DIR}/runtime/include)
  target_compile_features(dx8wasm_backend PUBLIC cxx_std_17)  # dx8wasm requires C++17
  # This target is defined before cmake/emscripten.cmake's add_compile_options(-pthread), so it
  # never inherits the project-wide pthread/atomics feature flags. That went unnoticed until
  # telemetry.cpp (std::atomic ring buffer): the final link uses --shared-memory for
  # PROXY_TO_PTHREAD, and wasm-ld refuses any object that emits atomic instructions without
  # having been compiled with the matching feature. Set it explicitly rather than relying on
  # inclusion order.
  target_compile_options(dx8wasm_backend PRIVATE -pthread)
  # Same inclusion-order trap as -pthread above, and for the same reason: this target is created
  # before cmake/emscripten.cmake runs its add_compile_options()/add_link_options(), so it
  # inherits none of the project-wide flags. Unexercised today only because the backend's sources
  # neither throw nor call zlib/FreeType directly — set them explicitly rather than relying on
  # include order, which a future reorder would silently break the same way.
  target_compile_options(dx8wasm_backend PRIVATE -fwasm-exceptions -sUSE_ZLIB=1 -sUSE_FREETYPE=1)
  target_link_options(dx8wasm_backend PRIVATE -fwasm-exceptions -sUSE_ZLIB=1 -sUSE_FREETYPE=1)
  message(STATUS "dx8wasm backend enabled (WASM) - implementation for the DXVK d3d8.h ABI")
endif()
