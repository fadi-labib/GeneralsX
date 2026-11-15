set(gamemath_SHARED_LIBS OFF CACHE BOOL "Force GameMath as static lib" FORCE)
set(GM_ENABLE_INTRINSICS OFF CACHE BOOL "Disable intrinsics for cross-arch determinism" FORCE)
set(GM_ENABLE_TESTS OFF CACHE BOOL "Disable GameMath tests" FORCE)

FetchContent_Declare(
    gamemath
    GIT_REPOSITORY https://github.com/TheSuperHackers/GameMath.git
    GIT_TAG        59f7ccd494f7e7c916a784ac26ef266f9f09d78d
)

FetchContent_MakeAvailable(gamemath)

# Ensure gamemath has position-independent code for 64-bit / PIE targets
if(TARGET gamemath)
    set_target_properties(gamemath PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# GeneralsX @build dx8wasm - WebAssembly has no floating-point exception flags, so Emscripten's
# fenv.h defines FE_ALL_EXCEPT as 0 and none of the individual flags. GameMath only raises, tests
# and clears them (never computes with them); as 0 those calls are no-ops and every result is
# unchanged, which is exactly the behaviour of the hardware they describe.
if(EMSCRIPTEN AND TARGET gamemath)
    target_compile_definitions(gamemath PRIVATE
        FE_INVALID=0 FE_DIVBYZERO=0 FE_OVERFLOW=0 FE_UNDERFLOW=0 FE_INEXACT=0)
endif()

# Ensure GameMath includes are available to ALL targets
# to prevent one-definition-rule violations and ensure USE_DETERMINISTIC_MATH activates consistently.
include_directories(${gamemath_SOURCE_DIR}/include)

