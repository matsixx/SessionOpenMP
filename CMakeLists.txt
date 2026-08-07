# SessionOpenMP -- Session: Skate Sim multiplayer as an overlay on N solo games.
#
# Quick start:
#   cmake --preset vs2022                     (writes build/SessionOpenMP.sln)
#   cmake --build build --config Release
# See README.md for the dependencies you must supply yourself.
cmake_minimum_required(VERSION 3.21)
project(SessionOpenMP C CXX)   # C too: MinHook is C, and a CXX-only project SILENTLY skips .c sources
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # /MT: no VC redist needed

# EOS 1.17.0 -- headers, import lib and the MATCHED runtime DLL all from the same SDK drop.
# This one cannot be fetched: the SDK is behind an Epic developer account. Point at your own copy
# with -DEOS_SDK=<path>, or set an EOS_SDK environment variable once and forget about it.
if(NOT EOS_SDK AND DEFINED ENV{EOS_SDK})
    set(EOS_SDK_DEFAULT "$ENV{EOS_SDK}")
else()
    set(EOS_SDK_DEFAULT "F:/SessionMPDev/EOS-SDK-39599718-v1.17.0/SDK")
endif()
set(EOS_SDK "${EOS_SDK_DEFAULT}" CACHE PATH "EOS SDK root")
if(NOT EXISTS "${EOS_SDK}/Include/eos_sdk.h")
    message(FATAL_ERROR
        "EOS SDK not found at '${EOS_SDK}'.\n"
        "Download EOS SDK 1.17.0 (C) from dev.epicgames.com and configure with -DEOS_SDK=<path to its SDK folder>.")
endif()

# EOS product credentials. The real header is gitignored, so a fresh clone has only the template --
# generate a placeholder copy rather than failing to compile. The build then works out of the box,
# the UDP and shared-memory backends work fully, and the EOS backend refuses at runtime with a
# message telling you to fill it in. An existing eos_creds.h is never overwritten.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/transport/eos_creds.h")
    configure_file("${CMAKE_SOURCE_DIR}/src/transport/eos_creds.h.template"
                   "${CMAKE_SOURCE_DIR}/src/transport/eos_creds.h" COPYONLY)
    message(STATUS "eos_creds.h generated from the template -- EOS networking is DISABLED until you "
                   "fill in your own credentials (see README.md). UDP and shared memory still work.")
endif()

add_library(omp_transport STATIC
    src/transport/transport.cpp          # the router (owns the log, dispatches the seam)
    src/transport/eos_transport.cpp      # between machines
    src/transport/shm_transport.cpp      # same PC -- the development rig
    src/transport/udp_transport.cpp)     # between machines, without Epic
target_include_directories(omp_transport PUBLIC src ${EOS_SDK}/Include)
target_link_libraries(omp_transport PUBLIC ${EOS_SDK}/Lib/EOSSDK-Win64-Shipping.lib)

add_executable(omp_ping tools/ping/ping_main.cpp)
target_link_libraries(omp_ping PRIVATE omp_transport)

# The matched runtime rides next to every binary that loads it.
add_custom_command(TARGET omp_ping POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${EOS_SDK}/Bin/EOSSDK-Win64-Shipping.dll" "$<TARGET_FILE_DIR:omp_ping>")

# M2: the replication core -- game-free, judged by omp_looptest before any game loads it.
add_library(omp_replication STATIC src/replication/replication.cpp src/replication/replaysync.cpp)
target_include_directories(omp_replication PUBLIC src)

add_executable(omp_looptest tools/looptest/loop_main.cpp)
target_link_libraries(omp_looptest PRIVATE omp_replication)

# M2.2: the game layer -- symbol table + its offline verifier (runs on disk, no game needed).
add_library(omp_game STATIC src/game/game_syms.cpp src/game/proxy.cpp src/game/gather.cpp src/ui/mp_name.cpp
                            src/ui/mp_prefs.cpp
                            src/game/cosmetics.cpp src/game/audio.cpp src/game/pose.cpp src/game/spectate.cpp)
target_include_directories(omp_game PUBLIC src)
target_link_libraries(omp_game PUBLIC omp_replication)

add_executable(omp_symcheck tools/symcheck/symcheck_main.cpp)
target_link_libraries(omp_symcheck PRIVATE omp_game)

# M2.3: session orchestration (transport <-> streams <-> proxies)
add_library(omp_session STATIC src/session/session.cpp src/session/banlist.cpp)
target_include_directories(omp_session PUBLIC src)
# NOT linked against omp_transport on purpose: session.cpp needs only the transport's HEADER, and
# omp_sessiontest substitutes its own Send/GetStats/PeerCount. Linking the real router here would drag
# its definitions into the test and collide with the mocks.
target_link_libraries(omp_session PUBLIC omp_replication omp_game)

# The session logic is game-free enough to test headless: peers arriving/going quiet/leaving.
add_executable(omp_sessiontest tools/sessiontest/session_main.cpp)
target_link_libraries(omp_sessiontest PRIVATE omp_session)

# The same-PC rig must be verifiable by one person alone: two real processes, no game, no EOS.
add_executable(omp_shmtest tools/shmtest/shm_main.cpp)
target_link_libraries(omp_shmtest PRIVATE omp_transport omp_replication)

# ...and so must the direct-UDP wire: two processes over loopback, still no game and no EOS.
add_executable(omp_udptest tools/udptest/udp_main.cpp)
target_link_libraries(omp_udptest PRIVATE omp_transport omp_replication)

# =====================================================================================================
# The UE4SS C++ mod  ->  <game>/SessionGame/Binaries/Win64/Mods/SessionOpenMP/dlls/main.dll
# UE4SS uses the DYNAMIC CRT (its dll imports MSVCP140/VCRUNTIME140) and CppUserModBase's ctor is
# imported from UE4SS.dll, so this target MUST be /MD while the tools stay /MT.
# =====================================================================================================
set(UE4SS_LIB "${CMAKE_SOURCE_DIR}/third_party/ue4ss/UE4SS.lib" CACHE FILEPATH "import lib generated from the installed UE4SS.dll")

if(EXISTS "${UE4SS_LIB}")
    add_library(omp_mod SHARED src/loader/ue4ss_mod.cpp)
    set_target_properties(omp_mod PROPERTIES
        OUTPUT_NAME "main"
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")          # /MD -- must match UE4SS
    target_compile_definitions(omp_mod PRIVATE RC_UE4SS_IMPORT UNICODE _UNICODE NOMINMAX OMP_USE_MINHOOK)
    target_include_directories(omp_mod PRIVATE src third_party/ue4ss_abi)
    # the static libs are /MT; rebuild the ones we need as /MD objects for this target instead of linking
    target_sources(omp_mod PRIVATE
        src/transport/transport.cpp
        src/transport/eos_transport.cpp
        src/transport/shm_transport.cpp
        src/transport/udp_transport.cpp
        src/replication/replication.cpp
        src/replication/replaysync.cpp
        src/game/game_syms.cpp
        src/game/proxy.cpp
        src/game/gather.cpp
        src/game/cosmetics.cpp
        src/game/audio.cpp
        src/game/pose.cpp
        src/game/spectate.cpp
        # Preferences are read by the LOADER before any transport starts, so they belong here and not
        # in the ImGui block below -- that block is conditional on the vendored sources existing.
        src/ui/mp_prefs.cpp
        src/session/session.cpp
        src/session/banlist.cpp)
    target_include_directories(omp_mod PRIVATE "${EOS_SDK}/Include")
    target_link_libraries(omp_mod PRIVATE "${UE4SS_LIB}" "${EOS_SDK}/Lib/EOSSDK-Win64-Shipping.lib")

    # ---- the in-game menu: ImGui on a hooked Present, MinHook for the hook itself.
    # Both are compiled straight INTO omp_mod rather than linked as libraries, so they inherit its /MD
    # runtime automatically (the tools are /MT; a mismatch here would corrupt on the first std:: type
    # that crossed the boundary).
    #
    # Fetched at the exact revisions this mod was developed and tested against. Point IMGUI_SRC /
    # MINHOOK_SRC at local checkouts to build offline or to try a different revision.
    set(IMGUI_SRC   "" CACHE PATH "Dear ImGui source (empty = fetch the pinned revision)")
    set(MINHOOK_SRC "" CACHE PATH "MinHook source (empty = fetch the pinned revision)")

    include(FetchContent)
    if(NOT IMGUI_SRC)
        FetchContent_Declare(imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG        cb16be3a3fc1f9cd146ae24d52b615f8a05fa93d)  # v1.90.9
        FetchContent_MakeAvailable(imgui)   # header-only drop: no CMakeLists, so this just populates
        set(IMGUI_SRC "${imgui_SOURCE_DIR}")
    endif()
    if(NOT MINHOOK_SRC)
        # SOURCE_SUBDIR names a directory that does not exist on purpose: MinHook ships a CMakeLists
        # that would build its own /MT library target, and we compile the .c files in ourselves.
        FetchContent_Declare(minhook
            GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
            GIT_TAG        d94c64d32ea37bc4f5ee47d580709f70c6fb6080   # v1.3.4+14
            SOURCE_SUBDIR  do-not-add-this-subdirectory)
        FetchContent_MakeAvailable(minhook)
        set(MINHOOK_SRC "${minhook_SOURCE_DIR}")
    endif()

    if(EXISTS "${IMGUI_SRC}/imgui.cpp" AND EXISTS "${MINHOOK_SRC}/src/hook.c")
        target_sources(omp_mod PRIVATE
            src/ui/overlay.cpp
            # The GAME'S OWN pause menu (round 369). No ImGui -- it lives here only because MinHook
            # is compiled in by this block, and it hooks the engine's menu code the same way.
            src/ui/pause_menu.cpp
            src/ui/version_tag.cpp
            src/ui/mp_name.cpp
            # Chat (round 382). It IS an ImGui window, so it belongs to this block -- and so does the
            # font hunt, whose only consumer is the chat atlas.
            src/ui/theme.cpp
            src/ui/chat.cpp
            # Floating player names. ImGui like the chat, and drawn from the same Present hook.
            src/ui/nameplates.cpp
            src/game/game_font.cpp
            ${IMGUI_SRC}/imgui.cpp
            ${IMGUI_SRC}/imgui_draw.cpp
            ${IMGUI_SRC}/imgui_tables.cpp
            ${IMGUI_SRC}/imgui_widgets.cpp
            ${IMGUI_SRC}/backends/imgui_impl_win32.cpp
            ${IMGUI_SRC}/backends/imgui_impl_dx11.cpp
            ${IMGUI_SRC}/backends/imgui_impl_dx12.cpp
            ${MINHOOK_SRC}/src/buffer.c
            ${MINHOOK_SRC}/src/hook.c
            ${MINHOOK_SRC}/src/trampoline.c
            ${MINHOOK_SRC}/src/hde/hde64.c)
        target_include_directories(omp_mod PRIVATE
            "${IMGUI_SRC}" "${IMGUI_SRC}/backends" "${MINHOOK_SRC}/include")
        target_link_libraries(omp_mod PRIVATE d3d11 d3d12 dxgi)
        message(STATUS "omp_mod: in-game menu (ImGui + MinHook) enabled")
    else()
        # Only reachable when IMGUI_SRC/MINHOOK_SRC were overridden with a wrong path -- an empty value
        # fetches. Fatal on purpose: a warning here used to produce a main.dll with no menu, no chat and
        # no pause-menu integration, which looks like a broken mod rather than a misconfigured build.
        message(FATAL_ERROR
            "omp_mod: ImGui/MinHook sources not found.\n"
            "  IMGUI_SRC   = '${IMGUI_SRC}' (needs imgui.cpp)\n"
            "  MINHOOK_SRC = '${MINHOOK_SRC}' (needs src/hook.c)\n"
            "Clear both to fetch the pinned revisions automatically.")
    endif()
    message(STATUS "omp_mod: UE4SS C++ mod target enabled")

    # =================================================================================================
    # SessionTweaks -- a SEPARATE UE4SS mod (-> Binaries/Win64/Mods/SessionTweaks/dlls/main.dll).
    # Gameplay fixes/improvements, not multiplayer: it enables/disables independently in mods.txt and
    # works without the MP mod at all (ini-only). Its F1 menu section rides omp_mod's menu_ext seam
    # at runtime. One TU per concern under src/tweaks/ (shell + scoop_speed + catch_tweaks).
    # Same /MD constraint as omp_mod (CppUserModBase ctor imported from UE4SS.dll); MinHook compiled
    # in as this DLL's OWN instance -- it hooks different functions than omp_mod, so no conflict.
    # No ImGui (the menu seam exists precisely so guests never link it), no EOS, no omp sources.
    # Both mod targets output "main.dll", so this one gets its own output directory.
    # =================================================================================================
    if(EXISTS "${MINHOOK_SRC}/src/hook.c")
        add_library(tweaks_mod SHARED
            src/tweaks/tweaks_mod.cpp
            src/tweaks/tweaks_common.cpp
            src/tweaks/scoop_speed.cpp
            src/tweaks/catch_tweaks.cpp
            src/tweaks/run_out.cpp
            src/tweaks/catch_level.cpp
            src/tweaks/catch_sound.cpp
            src/tweaks/grind_pop.cpp
            src/tweaks/cloth_physics.cpp
            ${MINHOOK_SRC}/src/buffer.c
            ${MINHOOK_SRC}/src/hook.c
            ${MINHOOK_SRC}/src/trampoline.c
            ${MINHOOK_SRC}/src/hde/hde64.c)
        set_target_properties(tweaks_mod PROPERTIES
            OUTPUT_NAME "main"
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tweaks")
        target_compile_definitions(tweaks_mod PRIVATE RC_UE4SS_IMPORT UNICODE _UNICODE NOMINMAX)
        target_include_directories(tweaks_mod PRIVATE src third_party/ue4ss_abi "${MINHOOK_SRC}/include")
        target_link_libraries(tweaks_mod PRIVATE "${UE4SS_LIB}")
        message(STATUS "tweaks_mod: SessionTweaks UE4SS mod target enabled")
    else()
        message(FATAL_ERROR "tweaks_mod: MinHook sources not found at MINHOOK_SRC='${MINHOOK_SRC}'")
    endif()
else()
    message(FATAL_ERROR
        "UE4SS import library not found at '${UE4SS_LIB}'.\n"
        "Generate it from your installed UE4SS.dll -- see third_party/ue4ss/README.md -- or pass\n"
        "-DUE4SS_LIB=<path>. Without it neither mod DLL can be built.")
endif()
