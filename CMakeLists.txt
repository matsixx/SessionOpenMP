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
    src/transport/udp_transport.cpp      # between machines, without Epic
    src/transport/relay_transport.cpp)   # between machines via a forwarding server (tools/relay)
target_include_directories(omp_transport PUBLIC src ${EOS_SDK}/Include)
target_link_libraries(omp_transport PUBLIC ${EOS_SDK}/Lib/EOSSDK-Win64-Shipping.lib)

# The relay server. Standalone by design: no game, no UE, no EOS -- it forwards opaque payloads and
# is meant to be built and run on a machine that has none of those. See docs/relay-scope.md.
add_executable(omp_relay tools/relay/relay_main.cpp)

add_executable(omp_ping tools/ping/ping_main.cpp)
target_link_libraries(omp_ping PRIVATE omp_transport)

# The matched runtime rides next to every binary that loads it.
add_custom_command(TARGET omp_ping POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${EOS_SDK}/Bin/EOSSDK-Win64-Shipping.dll" "$<TARGET_FILE_DIR:omp_ping>")

# M2: the replication core -- game-free, judged by omp_looptest before any game loads it.
add_library(omp_replication STATIC src/replication/replication.cpp src/replication/replaysync.cpp
                                   src/replication/dropsync.cpp src/replication/sidecar.cpp)
target_include_directories(omp_replication PUBLIC src)

add_executable(omp_looptest tools/looptest/loop_main.cpp)
target_link_libraries(omp_looptest PRIVATE omp_replication)

# M2.2: the game layer -- symbol table + its offline verifier (runs on disk, no game needed).
add_library(omp_game STATIC src/game/game_syms.cpp src/game/proxy.cpp src/game/gather.cpp src/ui/mp_name.cpp
                            src/ui/mp_prefs.cpp
                            src/game/cosmetics.cpp src/game/audio.cpp src/game/pose.cpp src/game/spectate.cpp
                            src/game/peer_bodies.cpp
                            src/game/teleport.cpp
                            src/game/trick_pulse.cpp
                            src/game/pa_state.cpp
                            src/game/custom_maps.cpp
                            src/game/dropper.cpp)
target_include_directories(omp_game PUBLIC src)
target_link_libraries(omp_game PUBLIC omp_replication)

add_executable(omp_symcheck tools/symcheck/symcheck_main.cpp)
target_link_libraries(omp_symcheck PRIVATE omp_game)

# M2.3: session orchestration (transport <-> streams <-> proxies)
add_library(omp_session STATIC src/session/session.cpp src/session/banlist.cpp src/session/modapi.cpp
                               src/session/replay_ghosts.cpp src/session/objhide.cpp)
target_include_directories(omp_session PUBLIC src)
# NOT linked against omp_transport on purpose: session.cpp needs only the transport's HEADER, and
# omp_sessiontest substitutes its own Send/GetStats/PeerCount. Linking the real router here would drag
# its definitions into the test and collide with the mocks.
target_link_libraries(omp_session PUBLIC omp_replication omp_game)

# The session logic is game-free enough to test headless: peers arriving/going quiet/leaving.
# update_check.cpp rides along for its version comparison: the popup's whole decision is "is theirs
# newer than mine", and that is pure logic worth gating even though the HTTP around it is not.
add_executable(omp_sessiontest tools/sessiontest/session_main.cpp src/ui/update_check.cpp)
target_link_libraries(omp_sessiontest PRIVATE omp_session)

# The same-PC rig must be verifiable by one person alone: two real processes, no game, no EOS.
add_executable(omp_shmtest tools/shmtest/shm_main.cpp)
target_link_libraries(omp_shmtest PRIVATE omp_transport omp_replication)

# ...and so must the direct-UDP wire: two processes over loopback, still no game and no EOS.
add_executable(omp_udptest tools/udptest/udp_main.cpp)
target_link_libraries(omp_udptest PRIVATE omp_transport omp_replication)

# SessionTweaks' emotes are geometry nobody can see from the source: which way a palm faces, whether a fist
# closes toward the palm, whether a dance leaves the feet where they were. Judged on a made-up skeleton with
# the game's own bone names, no game behind it. emote.cpp is compiled INTO the test, its callers stubbed.
if(MSVC)
    add_executable(omp_emotetest tools/emotetest/emote_test.cpp)
    target_include_directories(omp_emotetest PRIVATE src src/tweaks)
endif()

# What a lobby costs on the wire, from the REAL packer and the session's own send rules. A measurement, not a
# gate: it prints the table (omp_wirecost).
add_executable(omp_wirecost tools/wirecost/wire_cost.cpp)
target_include_directories(omp_wirecost PRIVATE src)
target_link_libraries(omp_wirecost PRIVATE omp_replication)

# SessionTweaks' signature search finds a function somebody has HOOKED (OpenMP's audio funnel hooks engine calls
# tweaks also calls): the real tweaks_common.cpp, one of this test's own functions hooked the way MinHook does.
if(MSVC)
    add_executable(omp_scantest tools/scantest/scan_test.cpp src/tweaks/tweaks_common.cpp)
    target_include_directories(omp_scantest PRIVATE src/tweaks src)
endif()

# The radio's message (src/tweaks/radio_wire.h): it is a stranger's bytes, so what is believed and what is dropped is
# checked here, with no game behind it.
add_executable(omp_radiotest tools/radiotest/radio_test.cpp)
target_include_directories(omp_radiotest PRIVATE src/tweaks src)
target_link_libraries(omp_radiotest PRIVATE omp_replication)

# The release FADE's arithmetic (src/game/pose_blend.h): letting go of a transported pose used to be a
# one-frame step, which every watcher saw as a twitch at the end of an emote. The fade that replaced it
# has two ways to reproduce that symptom -- discontinuity with the stamp, and the long way round the
# quaternion hypersphere -- so both are gated here, with no game behind it.
add_executable(omp_posetest tools/posetest/pose_test.cpp)
target_include_directories(omp_posetest PRIVATE src)

# Two EOS SDKs in one process: the game's own, and ours loaded beside it under another name (the mod no
# longer overwrites the game's -- that broke Epic DLC). Links NO import library on purpose: both copies are
# loaded by full path and bound by name, which is the whole point.  omp_eosdual <game's dll> <our dll>
if(MSVC)
    add_executable(omp_eosdual tools/eosdual/eos_dual.cpp)
    target_include_directories(omp_eosdual PRIVATE src ${EOS_SDK}/Include)
    # ...and the MECHANISM: the mod's own eos_sideload.cpp, linked the way main.dll is (the import library +
    # /DELAYLOAD), making ordinary EOS_* calls with the game's SDK already in the process.  omp_eosbind <game's dll>
    add_executable(omp_eosbind tools/eosdual/eos_bind.cpp src/transport/eos_sideload.cpp)
    target_include_directories(omp_eosbind PRIVATE src ${EOS_SDK}/Include)
    target_link_libraries(omp_eosbind PRIVATE ${EOS_SDK}/Lib/EOSSDK-Win64-Shipping.lib delayimp)
    target_link_options(omp_eosbind PRIVATE "/DELAYLOAD:EOSSDK-Win64-Shipping.dll")
    add_custom_command(TARGET omp_eosbind POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${EOS_SDK}/Bin/EOSSDK-Win64-Shipping.dll" "$<TARGET_FILE_DIR:omp_eosbind>/OMP_EOSSDK-Win64-Shipping.dll")
endif()

# The relay, end to end: this test LAUNCHES omp_relay and three real clients, so it depends on that
# binary existing beside it.
add_executable(omp_relaytest tools/relaytest/relay_test_main.cpp)
target_link_libraries(omp_relaytest PRIVATE omp_transport omp_replication)
add_dependencies(omp_relaytest omp_relay)

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
        src/transport/eos_sideload.cpp       # OUR EOS SDK beside the game's: see the /DELAYLOAD below
        src/transport/shm_transport.cpp
        src/transport/udp_transport.cpp
        src/transport/relay_transport.cpp
        src/replication/replication.cpp
        src/replication/replaysync.cpp
        src/replication/dropsync.cpp
        src/replication/sidecar.cpp
        src/game/game_syms.cpp
        src/game/dropper.cpp
        src/game/proxy.cpp
        src/game/gather.cpp
        src/game/cosmetics.cpp
        src/game/audio.cpp
        src/game/pose.cpp
        src/game/peer_bodies.cpp
        src/game/teleport.cpp
        src/game/trick_pulse.cpp
        src/game/pa_state.cpp
        src/game/custom_maps.cpp
        src/game/spectate.cpp
        # Preferences are read by the LOADER before any transport starts, so they belong here and not
        # in the ImGui block below -- that block is conditional on the vendored sources existing.
        src/ui/mp_prefs.cpp
        src/session/session.cpp
        src/session/banlist.cpp
        src/session/mutelist.cpp
        src/session/objhide.cpp
        src/session/modapi.cpp
        src/session/replay_ghosts.cpp
        src/loader/lua_api.cpp
        src/game/voice_audio.cpp
        src/voice/voice_capture.cpp
        src/voice/radio_capture.cpp)      # a player's radio: ONE app's sound, captured and streamed
    target_include_directories(omp_mod PRIVATE "${EOS_SDK}/Include")
    target_link_libraries(omp_mod PRIVATE "${UE4SS_LIB}" "${EOS_SDK}/Lib/EOSSDK-Win64-Shipping.lib")
    # THE GAME'S EOS SDK IS NEVER OVERWRITTEN (that broke Epic DLC). main.dll's EOS imports are DELAY-LOADED, and
    # src/transport/eos_sideload.cpp binds them to our own copy, shipped under another name beside main.dll:
    #     Mods/SessionOpenMP/dlls/OMP_EOSSDK-Win64-Shipping.dll
    # So main.dll loads with NO EOS dll to be found by name, and never binds to the game's by accident. Only this
    # target: the standalone tools (omp_ping ...) have no game beside them and link EOS the ordinary way.
    target_compile_definitions(omp_mod PRIVATE OMP_EOS_SIDELOAD)
    target_link_options(omp_mod PRIVATE "/DELAYLOAD:EOSSDK-Win64-Shipping.dll")
    target_link_libraries(omp_mod PRIVATE delayimp)
    add_custom_command(TARGET omp_mod POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${EOS_SDK}/Bin/EOSSDK-Win64-Shipping.dll" "$<TARGET_FILE_DIR:omp_mod>/OMP_EOSSDK-Win64-Shipping.dll")

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
            GIT_TAG        3369cbd2776d7567ac198b1a3017a4fa2d547cc3)  # v1.90.9-docking (multi-viewport: the F1 pop-out)
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
        # Opus -- the voice codec (BSD). Built from source here so its objects can be /MD like omp_mod
        # (the top-level default is /MT for the tools, which cannot link into UE4SS's process).
        set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
        set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(opus
            GIT_REPOSITORY https://github.com/xiph/opus.git
            GIT_TAG        v1.5.2
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(opus)
        set_target_properties(opus PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
        target_include_directories(omp_mod PRIVATE "${opus_SOURCE_DIR}/include")
        target_link_libraries(omp_mod PRIVATE opus ole32)
        target_compile_definitions(omp_mod PRIVATE OMP_VOICE=1)   # session.cpp's voice half; the tools' library build has none
        # The radio's app capture, tested for real: a copy of the test plays a tone, the other captures that
        # process alone and measures it. /MD like opus. Makes a faint beep; needs Windows 11 (or 10 build 20348+).
        add_executable(omp_radiocaptest tools/radiocaptest/radio_cap_test.cpp src/voice/radio_capture.cpp)
        set_target_properties(omp_radiocaptest PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
        target_include_directories(omp_radiocaptest PRIVATE src "${opus_SOURCE_DIR}/include")
        target_link_libraries(omp_radiocaptest PRIVATE opus ole32)
        # A MEASUREMENT, not a gate: streaming an app also plays it out of the streamer's own speakers.
        # Can the source be silenced LOCALLY and still be captured? Answered with a tone, not guessed at.
        add_executable(omp_radiosolo tools/radiosolo/radio_solo.cpp src/voice/radio_capture.cpp)
        set_target_properties(omp_radiosolo PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
        target_include_directories(omp_radiosolo PRIVATE src "${opus_SOURCE_DIR}/include")
        target_link_libraries(omp_radiosolo PRIVATE opus ole32)
        set(MINHOOK_SRC "${minhook_SOURCE_DIR}")
    endif()

    if(EXISTS "${IMGUI_SRC}/imgui.cpp" AND EXISTS "${MINHOOK_SRC}/src/hook.c")
        target_sources(omp_mod PRIVATE
            src/ui/overlay.cpp
            # The GAME'S OWN pause menu (round 369). No ImGui -- it lives here only because MinHook
            # is compiled in by this block, and it hooks the engine's menu code the same way.
            src/ui/pause_menu.cpp
            src/ui/version_tag.cpp
            # The GAME'S OWN popup, and the release check that uses it. Here rather than in the
            # always-on list because the popup hooks nothing but reaches the game instance the
            # version tag captured -- they are two halves of one thing.
            src/ui/trx_popup.cpp
            src/ui/whats_new.cpp
            src/ui/nacon_panel.cpp
            src/ui/news_panel.cpp
            src/ui/update_check.cpp
            src/ui/mp_name.cpp
            # Chat (round 382). It IS an ImGui window, so it belongs to this block -- and so does the
            # font hunt, whose only consumer is the chat atlas.
            src/ui/theme.cpp
            src/ui/chat.cpp
            # Floating player names. ImGui like the chat, and drawn from the same Present hook.
            src/ui/nameplates.cpp
            # ...and the same names drawn by the GAME instead, in its own text widget. No ImGui at
            # all -- it is here only because it shares the nameplate tuning next door.
            src/ui/game_hud.cpp
            src/game/game_font.cpp
            ${IMGUI_SRC}/imgui.cpp
            ${IMGUI_SRC}/imgui_draw.cpp
            ${IMGUI_SRC}/imgui_tables.cpp
            ${IMGUI_SRC}/imgui_widgets.cpp
            ${IMGUI_SRC}/backends/imgui_impl_win32.cpp
            ${IMGUI_SRC}/backends/imgui_impl_dx11.cpp
            # The DX12 backend is OUR COPY of the pinned one: identical for the game's swapchain, but
            # the F1 pop-out window does not stall the render thread waiting on the GPU (see its header).
            src/ui/imgui_impl_dx12_omp.cpp
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
    src/tweaks/flip_speed.cpp
            src/tweaks/catch_tweaks.cpp
            src/tweaks/run_out.cpp
            src/tweaks/catch_level.cpp
            src/tweaks/catch_sound.cpp
            src/tweaks/cloth_merge.cpp
            src/tweaks/cloth_sim.cpp
            src/tweaks/pitch_range.cpp
            src/tweaks/foot_place.cpp
            src/tweaks/board_stance.cpp
            src/tweaks/foot_steer.cpp
            src/tweaks/grind_pop.cpp
            src/tweaks/grind_rock.cpp
            src/tweaks/grind_lean.cpp
            src/tweaks/pop_probe.cpp
            src/tweaks/body_feel.cpp
            src/tweaks/proxy_body_feel.cpp
            src/tweaks/camera_height.cpp
            src/tweaks/cam_fp.cpp
            src/tweaks/upscale.cpp
            src/tweaks/max_detail.cpp
            src/tweaks/upscale_fsr.cpp
            src/tweaks/sit.cpp
            src/tweaks/sit_ui.cpp
            src/tweaks/radial.cpp
            src/tweaks/emote.cpp
            src/tweaks/radio.cpp
            ${MINHOOK_SRC}/src/buffer.c
            ${MINHOOK_SRC}/src/hook.c
            ${MINHOOK_SRC}/src/trampoline.c
            ${MINHOOK_SRC}/src/hde/hde64.c)
        set_target_properties(tweaks_mod PROPERTIES
            OUTPUT_NAME "main"
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tweaks")
        target_compile_definitions(tweaks_mod PRIVATE RC_UE4SS_IMPORT UNICODE _UNICODE NOMINMAX)
        target_include_directories(tweaks_mod PRIVATE src third_party/ue4ss_abi third_party/ffx-api "${MINHOOK_SRC}/include")
        target_link_libraries(tweaks_mod PRIVATE "${UE4SS_LIB}")
        message(STATUS "tweaks_mod: SessionTweaks UE4SS mod target enabled")
    else()
        message(FATAL_ERROR "tweaks_mod: MinHook sources not found at MINHOOK_SRC='${MINHOOK_SRC}'")
    endif()

    # ---- OmpModTest: an in-game test of the mod channel, written like a third-party mod (it sees only
    # sdk/omp_mod_api.h). Not shipped. -> build/modtest/Release/main.dll
    add_library(omp_modtest SHARED tools/modtest/modtest_mod.cpp)
    set_target_properties(omp_modtest PROPERTIES
        OUTPUT_NAME "main"
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modtest")
    target_compile_definitions(omp_modtest PRIVATE RC_UE4SS_IMPORT UNICODE _UNICODE NOMINMAX)
    target_include_directories(omp_modtest PRIVATE sdk third_party/ue4ss_abi)
    target_link_libraries(omp_modtest PRIVATE "${UE4SS_LIB}")
else()
    message(FATAL_ERROR
        "UE4SS import library not found at '${UE4SS_LIB}'.\n"
        "Generate it from your installed UE4SS.dll -- see third_party/ue4ss/README.md -- or pass\n"
        "-DUE4SS_LIB=<path>. Without it neither mod DLL can be built.")
endif()
