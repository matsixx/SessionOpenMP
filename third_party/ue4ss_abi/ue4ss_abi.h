// SessionOpenMP -- a self-contained, ABI-faithful declaration of RC::CppUserModBase.
//
// ⚠️ PINNED TO UE4SS **v3.0.1 STABLE** (the release build installed on both Epic and Steam, and the one
// the SessionCoop package ships to joiners). Transcribed from the user's copy of the release source:
//   C:\Users\lilma\Downloads\RE-UE4SS-3.0.1\RE-UE4SS-3.0.1\UE4SS\include\Mod\CppUserModBase.hpp
//
// WHY NOT INCLUDE THE REAL HEADER: it reaches imgui/fmt through GUI/Console.hpp and
// DynamicOutput/Output.hpp, and stubbing that chain is an expanding chase. The ABI we must agree on is
// only (a) the vtable slot order and (b) where our derived fields begin -- so the same class, same
// members in the same order, same virtuals in the same order, is transcribed here instead.
//
// ⛔ THIS FILE HAS ALREADY CAUSED ONE CRASH AND HIDDEN ONE BUG -- both from transcribing the WRONG
// VERSION's header. Do not "remember" this class; READ the installed build's header every time:
//   1. (2026-07-30) The installed UE4SS was briefly the EXPERIMENTAL branch, whose class has FIVE more
//      virtuals (four Lua*-taking on_lua_start/on_lua_stop overloads + on_cpp_mods_loaded). UE4SS fired
//      on_cpp_mods_loaded() right after start_cpp_mods() -- a vcall past the end of the transcribed
//      vtable -- AV inside UE4SS.dll before Unreal init, game closed instantly, deterministic.
//   2. The first transcription ALSO contained on_ui_init(), which does NOT exist in stable 3.0.1 (it is
//      an experimental addition). Every slot after on_unreal_init sat one off -- all landing on no-ops,
//      so it never crashed, but on_dll_load/render_tab would have silently misfired. A partial or
//      mixed-version transcription is wronger than it looks.
//
// VERIFY-ON-UPDATE: if UE4SS is ever upgraded, re-read the real header and re-check the virtual ORDER
// and member list against this file. The stable-3.0.1 declaration order is:
//   ~dtor, on_update, on_unreal_init, on_program_start,
//   on_lua_start x2, on_lua_stop x2, on_dll_load, render_tab
// (MSVC groups virtual OVERLOADS adjacently at the first overload's position in reverse declaration
// order; both sides compile from the same declaration order, so the grouping matches by construction.)
//
// Notes on members:
//   * `GUI::GUITab` stays INCOMPLETE on purpose: the member is
//     `std::vector<std::shared_ptr<GUI::GUITab>>`, whose layout does not depend on T.
//   * `StringType` is `std::wstring` (UE4SS builds wide on Windows) -- the five metadata strings are
//     assigned by our constructor and read by UE4SS. Safe across the boundary because BOTH sides use
//     the DYNAMIC CRT (verified: the release UE4SS.dll imports MSVCP140/VCRUNTIME140/ucrt) -- one heap.
//   * The ctor/dtor are IMPORTED from UE4SS.dll (??0CppUserModBase@RC@@QEAA@XZ / ??1...@UEAA@XZ,
//     both verified exported by the stable release), which is why the mangled names must come out
//     identical -- they do, because the class name, namespace and signatures match.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RC
{
namespace GUI { class GUITab; }                       // deliberately incomplete
namespace LuaMadeSimple { class Lua; }

using StringType     = std::wstring;
using StringViewType = std::wstring_view;

class __declspec(dllimport) CppUserModBase
{
  protected:
    std::vector<std::shared_ptr<GUI::GUITab>> GUITabs{};

  public:
    StringType ModName{};
    StringType ModVersion{};
    StringType ModDescription{};
    StringType ModAuthors{};
    StringType ModIntendedSDKVersion{};

  public:
    CppUserModBase();
    virtual ~CppUserModBase();

    virtual auto on_update() -> void {}
    virtual auto on_unreal_init() -> void {}
    virtual auto on_program_start() -> void {}

    // The two on_lua_start / on_lua_stop overloads take Lua references we never touch; only their SLOTS
    // matter, so the parameter types are declared opaquely but in the correct order and count.
    virtual auto on_lua_start(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&,
                              LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) -> void {}
    virtual auto on_lua_start(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&,
                              LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) -> void {}
    virtual auto on_lua_stop(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&,
                             LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) -> void {}
    virtual auto on_lua_stop(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&,
                             LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) -> void {}
    virtual auto on_dll_load(StringViewType) -> void {}
    virtual auto render_tab() -> void {}
};

// ---- the one piece of UE4SS's Unreal API we use: find an object by class name.
// Mangled target (verified present in the STABLE release UE4SS.dll's exports):
//   ?FindFirstOf@UObjectGlobals@Unreal@RC@@YAPEAVUObject@23@PEB_W@Z
// i.e. RC::Unreal::UObjectGlobals::FindFirstOf(const wchar_t*) -> RC::Unreal::UObject*, __cdecl.
// Declaring it with exactly this namespace/signature reproduces that name, so it links against the
// generated import lib with no headers from the source tree.
// UObject stays INCOMPLETE: we only ever pass the pointer around and read raw offsets off it.
namespace Unreal
{
class UObject;
class UFunction;
namespace UObjectGlobals
{
    __declspec(dllimport) UObject* FindFirstOf(const wchar_t* class_name);
    // Round 382: every object of a class, for the one-shot font hunt (there are a handful of
    // UFontFace objects and we want to see them all before choosing). Mangled target, verified in
    // UE4SS.def line 841:
    //   ?FindAllOf@UObjectGlobals@Unreal@RC@@YAXPEB_WAEAV?$vector@PEAVUObject@Unreal@RC@@...
    // The std::vector crosses the DLL boundary, which is legal here for the same reason the
    // std::function above is: this target is the /MD one, sharing UE4SS's CRT.
    __declspec(dllimport) void FindAllOf(const wchar_t* class_name, std::vector<UObject*>& out);
}

// ---- the GAME-THREAD ANCHOR: UE4SS's ProcessEvent pre-callback registration.
// Mangled target (verified exported by the stable release DLL):
//   ?RegisterProcessEventPreCallback@Hook@Unreal@RC@@YAXV?$function@$$A6AXPEAVUObject@Unreal@RC@@PEAVUFunction@23@PEAX@Z@std@@@Z
// i.e. void RC::Unreal::Hook::RegisterProcessEventPreCallback(std::function<void(UObject*, UFunction*, void*)>).
// This is EXACTLY what UE4SS's own Lua native RegisterHook rides (LuaMod.cpp:3085) -- and the user's
// proven config already exercises it (CheatManagerEnabler's "Registered native hook (1, 2)" log line),
// so the underlying ProcessEvent detour is known-installed and known-stable on Session.
// std::function crosses the DLL boundary by value: safe because BOTH sides are /MD Release (one CRT,
// one STL, _ITERATOR_DEBUG_LEVEL 0 -- the same argument as the metadata strings).
// There is NO unregister export: the callback must be a function whose lifetime is the process (ours is;
// C++ mods only unload at process shutdown).
namespace Hook
{
    __declspec(dllimport) void RegisterProcessEventPreCallback(std::function<void(UObject*, UFunction*, void*)>);
}
} // namespace Unreal

} // namespace RC

#define STR(x) L##x
