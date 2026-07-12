set(GS_OPENSSL FALSE)
set(GAMESPY_SERVER_NAME "server.cnc-online.net")

FetchContent_Declare(
    gamespy
    GIT_REPOSITORY https://github.com/TheAssemblyArmada/GamespySDK.git
    GIT_TAG        07e3d15c500415abc281efb74322ab6d9c857eb8
)

FetchContent_MakeAvailable(gamespy)

# GeneralsX @build dx8wasm - GameSpy selects its OS via _UNIX/_LINUX (derived from
# __linux__/__APPLE__), which emscripten doesn't define, so it hits an #error.
# Emscripten is POSIX with BSD sockets (WebSocket-backed at runtime), so route it
# through GameSpy's Unix path. Multiplayer runtime itself is a later plan; this
# just lets the SDK compile+link. PUBLIC so GameEngine TUs including GameSpy
# headers inherit the same platform selection.
if(EMSCRIPTEN)
    # GameSpy builds one static lib per component; set the platform selector on
    # each. The GIT_TAG above is pinned, so this list is stable.
    foreach(_gs gscommon gscdkey gschat gsgp gsgstats gsgt2 gshttp gsinterface
                gsnatneg gspeer gspinger gspt gsqr gsqr2 gssake gssc
                gsserverbrowsing gsvoice2 gswebservices gamespy)
        if(TARGET ${_gs})
            # _UNIX/_LINUX drive the GameSpy headers; some .c files switch on the
            # __linux__ builtin directly, so set that too. Emscripten's own musl
            # headers key on __EMSCRIPTEN__, not __linux__, so this only steers
            # GameSpy's platform picks toward its POSIX path.
            get_target_property(_gs_type ${_gs} TYPE)
            if(_gs_type STREQUAL "INTERFACE_LIBRARY")
                target_compile_definitions(${_gs} INTERFACE _UNIX _LINUX __linux__)
            else()
                target_compile_definitions(${_gs} PUBLIC _UNIX _LINUX __linux__)
            endif()
        endif()
    endforeach()
endif()
