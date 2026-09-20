// SessionOpenMP -- co-op multiplayer for Session, as an overlay on N solo games.
// Copyright (C) 2026 matsix
//
// This program is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version. It is
// distributed WITHOUT ANY WARRANTY; see the GNU GPL (LICENSE) for details.
//
// Additional permission under GNU GPL version 3 section 7: you may link and convey this
// work combined with the Epic Online Services SDK and the proprietary game runtime it
// loads into. See LICENSE-EXCEPTION.txt.
// SessionTweaks -- THE RADIAL MENU. Contract: radial.h.
//
// WHERE THE INPUT COMES FROM. Two seams this mod already owns, and nothing new is hooked:
//   * UPlayerInput::InputKey (catch_tweaks.cpp) carries every button AND the sticks' axis events. The
//     right-stick click opens the wheel; while it is open A, B and the click are swallowed, and the
//     right stick's axis events are passed on REWRITTEN TO ZERO. Zero, not swallowed: a swallowed axis
//     leaves the game holding the last value it saw, and a camera that was turning when the wheel
//     opened would keep turning for as long as it stayed open.
//   * InputHandler::Tick (scoop_speed.cpp) hands over the tick's own stick buffer. The right stick is
//     read from it -- that is what points at an entry -- and zeroed in it while the wheel is open, for
//     whatever reads the handler rather than the player input.
// The LEFT stick is not touched in either place, which is the whole of "you can still walk".
//
// WHAT IT IS DRAWN WITH. The game's own button widget with its glyph collapsed (sit_ui.h, free
// labels), one per entry, placed on a circle. The game has no wheel art, so the wheel is the words
// and which one is lit. The A/B hints are the same prompt bar sitting uses, which this owns while open.
//
// WHAT THE ENTRIES DO.
//   NOTHING ON THE WHEEL SELLS ANYTHING. Buying stays where the game put it, at a shop in a level. The
//   wheel's Closet and Board are for what you already own, and both are built on the same thing:
//   * THE SHOP UNDER BOTH -- a shop of OUR OWN, spawned where you stand, and the game's own confirm handler called
//     on it: ASkateShop::HandleOnInteractionPromptConfirm, which is what runs when you press the button
//     at a counter. The actor is small -- a trigger, the spots the preview skater and board stand on,
//     and the shop's cameras, no building -- and it carries everything the menu is built from, so a map
//     needs no shop of its own. The handler's first act is to read `_collidingSkater` (the skater at
//     the counter) and use that skater's controller, so it is filled in first; called with it empty it
//     faults. Ours is destroyed once the menu has closed and the camera has had time to blend back;
//     left behind it would be a phantom shop prompting wherever you last shopped.
//     OURS IS NEVER A SHOP YOU CAN WALK UP TO. It is still a real ASkateShop, and one standing on the
//     spot you are standing on: left to itself it sees you in its trigger, shows its prompt, and opens
//     as an ordinary BUYING shop -- and in the seconds before it is destroyed that is exactly what
//     happened, after which it was destroyed underneath its own open menu and the next key press read
//     its dead camera. So: its collision is switched off the moment it exists, which is the end of its
//     trigger; when its menu closes the skater at its counter is cleared, along with the flag its tick
//     shows a prompt from, and any prompt it has is taken down; and it is never destroyed while it has
//     a menu up -- if one ever appears again it is treated as ours, shown what you own and kept alive.
//     TWO THINGS A LEVEL DOES FOR A PLACED SHOP, WHICH OURS HAS TO DO FOR ITSELF. (1) Layout: in the
//     blueprint every component sits at the actor's origin, and the designers move the cameras and the
//     spots per shop. Ours is laid out around where you stand -- and the apparel camera's recorded base
//     position is overwritten too, because BeginPlay captures it in WORLD space at spawn and the shop's
//     tick keeps steering the camera back to it. (2) Input: the menu takes its keys as an ordinary
//     widget, which needs keyboard focus, and nothing in the game's native code ever gives it any; out
//     of the normal flow it arrives without, and B falls through to the game. It is given focus the
//     moment it appears. Closing is the game's own: it hands focus back to the viewport itself.
//     FOCUS IS KEPT, NOT JUST GIVEN. The shop's input mode is "game and UI": whatever the focused widget
//     does not take falls through to the game, and when the widget holding focus goes away -- buying
//     and equipping rebuilds the pages under it -- focus lands on the viewport and the menu, still on
//     screen, stops hearing anything. So while it is up it is asked a few times a second whether it
//     still has focus, and handed it back if not. NOT WHILE A SUB-SCREEN IS OPEN: the colour picker and
//     the wheel-orientation screen are menus of their own that need the focus themselves, and each
//     pushes an entry on the game's input-mode stack, so a stack deeper than it was when the shop
//     came up means somebody else rightly holds it.
//   * Emotes -- a second wheel, whose entries are whatever emote.cpp performs (the game ships no emotes
//     and no animations for one, so they are played on the skeleton itself). Picking one closes the
//     wheel and plays it. This wheel opens from a SEAT too: an emote sits on top of a seated pose.
//   * Closet -- THE SAME SHOP, SHOWING WHAT YOU OWN. The apartment's closet is a widget of its own
//     (UCustomizationMenuPageContainer) and cannot run anywhere else: its setup casts the owning
//     controller to the apartment's AMainHUBPlayerController and, when the cast fails, sets the pointer
//     to null and reads a field off it on the very next instruction. In a skate map that is a fault,
//     every time. But what makes a closet a closet is not that widget, it is the customization actor's
//     MODE: ACharacterCustomization::GetFilteredCustomizationItems skips everything you do not own when
//     `_currentMode` is 0 and lists the catalogue when it is 1, and the shop sets 1 as its menu is built
//     (the apartment never sets it). So the closet opens the store exactly as above and, the moment
//     the menu is up and before any category has built its list, puts the actor back to 0, marks its
//     cached list dirty and gives the item grid the matching mode -- through UWidgetCustomizationGrid::
//     SetGridMode, which also shows and hides the price parts. Every item then listed is one you own.
//     CONFIRM WEARS IT, AND NEVER BUYS. The shop's menu knows one thing to do with a confirmed item: if
//     it judges it purchasable it opens the buy page, and its purchase code adds the item to your
//     inventory and takes the price BEFORE it equips -- so a closet that let that run would sell you a
//     second copy of something you own for nothing. Its key handler is therefore detoured, and a
//     confirm does what the APARTMENT'S closet does, read out of its own key handler:
//     ACharacterCustomization::SetProfileItem(item, FALSE), then the grid's SetSelected_Checked with the
//     item's own "is on" bit (+8, bit 0) as that call left it. The shop's purchase passes TRUE, meaning
//     a fresh purchase, and the game then settles on an instance of the item by itself: with several
//     colours of one garment owned, the one you picked was not reliably the one you got. FALSE wears the
//     very instance the list entry stands for.
//     The menu's "purchasable" flag is then cleared for that one call and the game's own handler runs:
//     with nothing to buy its confirm path is a plain Handled -- no page, no sound -- which is exactly
//     the reply wanted, built by the game rather than by us.
//     WHAT CAN BE WORN IS WHATEVER IS LISTED. The confirm is NOT tied to the menu's "purchasable" flag:
//     that flag also demands you own fewer than the item's maximum, which is never true of a one-off or
//     a DLC piece, so tying to it made exactly those impossible to put on. The test is the one the
//     eye makes -- the item grid is showing and a real item is selected -- on the key the game's own
//     confirm branch tests for, and not on a repeat. The flag is cleared on EVERY key in these menus,
//     worn or not, so the buy page cannot open from anything in them.
//     WHAT YOU HOVER IS PREVIEWED AS THE ONE YOU OWN. The shop's hover handler
//     (CharacterCustomization_UpdateDetails) copies the list entry and then OVERWRITES the copy's colour
//     (+0xc) and custom-colour blocks with the MENU'S own `_currentVariantId` and colour state before it
//     previews it -- right for a catalogue, whose entries are generic and whose colour you cycle in the
//     menu, and that id goes back to -1 every time the selection moves. In the owned list there is one
//     entry per copy you own, each carrying its own colour, so every colour of a garment previewed the
//     same, and since a confirm does not redraw the character, what you saw after picking one was still
//     that preview: "it is not selecting the right pants". The apartment's handler previews the entry
//     untouched, with SampleCustomizationItem's third argument TRUE (keep the item's own attributes when
//     the materials are mapped; the shop passes FALSE). So the shop's handler is detoured and, in these
//     menus, the entry is previewed again the apartment's way straight after: the second request cancels
//     the first's load. The "nothing" entry (the actor's `_customizationItemNone`) is left to the game --
//     both handlers build that one from the skater and the flag makes no difference to it.
//     IT OPENS ON ITS OWN PAGE. A shop placed in a level may name the page its menu starts on
//     (`_skateShopRootPage`); ours names the page behind one item of the menu blueprint's root page,
//     found by that item's key -- "SkateShopBuySkaterGear" for the clothes. If the page is not found the
//     menu is not opened at all: the fallback would be the shop's "buy ..." front page.
//     NOT HERE: selling, and the apartment closet's "select / edit skater" and "create a skater" --
//     those live in the apartment's widget and work through its controller.
//   * Board -- the same again on the page behind "SkateShopBuySkateboardGear": the board parts you own.
//     Fitting wheels opens the game's wheel-orientation screen, as buying them at a shop does.
//     ITS CAMERA IS SWITCHED ON BY US. The shop changes camera in one place only,
//     USkateShopMenuPageContainer::OnPageSelectionConfirmed, as an entry of its FRONT page is confirmed
//     -- and these menus open past the front page, so the view stayed on the overview camera with the
//     board a small thing by the skater's shin. ASkateShop::ActivateSkateboardGearCamera is therefore
//     called as the Board menu comes up. WHERE THAT CAMERA STANDS IS READ, NOT GUESSED: the board is sent
//     to `_customizationSkateboardRoot` plus a per-category offset from the blueprint's
//     `_skateboardSettings` (ASkateShop::SetSkateboardSettings -> StartSkateboardTransition, as each
//     category's grid opens), and a first guess at where that lands was wrong by most of a metre. So
//     while the Board menu is up the gear camera is eased, every frame, to stand in front of where the
//     customization skateboard (`ACharacterCustomization::_customizationSkateboard`) actually is.
//     THE BOARD'S ANCHOR IS TURNED TO FACE THAT CAMERA, AND HAD TO BE MADE MOVABLE FIRST. The log of the
//     round above showed the board resting exactly 40 right and 40 up of the FEET: the anchor
//     (`_customizationSkateboardRoot`) had never gone where LayOut put it. It is a plain scene component
//     that is not Movable, and USceneComponent::CheckStaticMobilityAndWarn refuses, silently in a
//     shipping build, to move one of those once play has begun -- the cameras are Movable, which is why
//     they obeyed. So the anchor is given USceneComponent::SetMobility(Movable) before it is placed.
//     And it is placed a QUARTER TURN to the left of the way you face. Each category's RotationOffset
//     presents the board to a camera standing on the anchor's RIGHT (Deck Graphics stands it on its tail
//     with the graphic facing that way; the offsets are all 0 forward, 40-60 right: toward that camera).
//     Seen from the front instead, the graphic faced sideways -- and the sticks lost an axis, which is
//     how it was reported: SkateboardMesh_AddYawRotation turns the board about ITS OWN Z, the deck's
//     normal (cur * yaw), and AddPitchRotation about the VIEW TARGET'S right vector (pitch * cur); with
//     the normal lying along the camera's right those are one and the same line. Turned, the anchor's
//     right is the way you face, where our camera stands: the graphic faces it, one stick spins it and
//     the other tilts it, as in a shop. Board only -- the closet's board stays at your feet, as tested.
//   * Props -- present, not wired.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "tweaks_common.h"
#include "radial.h"
#include "sit.h"
#include "sit_ui.h"
#include "emote.h"          // the second wheel's entries, and what picking one does
#include "radio.h"           // the Props wheel: the radio
#include "catch_tweaks.h"   // CatchTweaks_Skater
#include "foot_place.h"     // FootPlace_AnimInstance
#include "grind_pop.h"      // GrindPop_FNameToString
#include "ui/menu_ext.h"
#include "MinHook.h"

// ------------------------------------------------------------------ knobs
static int   g_on = 1;                  // RadialEnabled
// Layout numbers are read from the ini but never written to it: they are being calibrated against
// screenshots, and a guess saved to disk outlives the build that guessed it.
static float g_radiusPct = 24.0f;       // RadialRadiusPct: of the viewport's shorter side
static float g_deadzone  = 0.45f;       // RadialDeadzonePct: how far the stick travels before it points
static float g_charW     = 16.0f;       // RadialCharW: see SitUI_SetFreeMetrics
static float g_dim       = 0.9f;        // RadialDimPct: what is lit is said with colour, not by fading the rest
// The Board menu's camera, from the board: how far in front of it (toward where you were facing), how
// far above it, and how far to your right of it. Read from the ini, never written.
static float g_boardCamDist = 120.0f;   // RadialBoardCamDist
static float g_boardCamUp   = 10.0f;    // RadialBoardCamUp
static float g_boardCamSide = 0.0f;     // RadialBoardCamSide
// ...and the board's anchor, from your feet: the board then comes 40-60 further FORWARD of it and 30-40
// higher, by the shop's own per-category offsets. Up is there to keep a board standing on its tail off
// rising ground.
static float g_boardRootFwd   = -10.0f; // RadialBoardRootFwd
static float g_boardRootRight = 75.0f;  // RadialBoardRootRight
static float g_boardRootUp    = 25.0f;  // RadialBoardRootUp

enum { AN_ON_BOARD   = 0x300,           // USkaterAnimInstance::IsOnBoard -- riding, not walking
       SHOP_CUSTOM_BP = 0x268,          // ASkateShop::_characterCustomization_Blueprint
       SHOP_MENU_BP   = 0x270,          //   ..._menuPageContainerBlueprint
       SHOP_ROOT_PAGE = 0x278,          //   ..._skateShopRootPage
       SHOP_COLLIDER  = 0x320,          //   ..._collidingSkater: the skater at the counter
       SHOP_PROMPT    = 0x328,          //   ..._interactionPrompt: non-null while its prompt is on screen
       SHOP_PROMPT_DUE = 0x340,         //   ..._isDisplayPromptRequired: its tick shows a prompt when this is set
       ITEM_FLAGS8    = 0x08,           // FSkateFilteredItem: bit 0 = this one is on
       SHOP_MENU      = 0x330,          //   ..._skateShopMenu: non-null while the menu is up
       SHOP_BOARD_ROOT = 0x238,         //   ..._customizationSkateboardRoot (USceneComponent*)
       SHOP_SKATER_ROOT = 0x240,        //   ..._customizationSkaterRoot
       SHOP_CAMERA    = 0x248,          //   ..._skateShopCamera (UCameraComponent*): the overview
       SHOP_CAM_APPAREL = 0x250,        //   ..._skateShopSkaterApparelCamera (UChildActorComponent*)
       SHOP_CAM_GEAR  = 0x258,          //   ..._skateShopSkateboardGearCamera
       SHOP_CAM_DIY   = 0x260,          //   ..._skateShopDIYCamera
       SHOP_OBJ_ROOT  = 0x2b0,          //   ..._objectPlacementRoot
       SHOP_APPAREL_BASE = 0x37c,       //   ..._skaterApparelCameraBasePosition: WORLD, captured at BeginPlay
       SHOP_BOARD_BASE = 0x394,         //   ..._skateboardBasePosition
       COMP_REL_LOC   = 0x11c,          // USceneComponent::RelativeLocation, then RelativeRotation +0x128
       SHOPMENU_CAN_BUY = 0x420,        // USkateShopMenuPageContainer::_isPurchaseAvailable
       SHOPMENU_CATEGORY = 0x414,       //   ..._cachedCategoryID; its low word is the category, 0x100 = wheels
       GRID_VISIBILITY = 0xc3,          // UWidget::Visibility on the grid: 0 = visible, an item list is up
       GRID_ITEM_COUNT = 0x368,         // UWidgetCustomizationGrid::_itemCount
       KEYEVENT_REPEAT = 0x0a,          // FInputEvent::bIsRepeat
       MENU_ROOT_PAGE = 0x260,          // UMenuPageContainer::_rootPageDefinition (UMenuPageDefinition*)
       PAGE_ITEMS     = 0x58,           // UMenuPageDefinition::_menuItems (TArray<FMenuPageItemDefinition>)
       ITEM_SIZE      = 0x90,           // FMenuPageItemDefinition: _key +0, _subPageDefinition +0x58
       ITEM_SUBPAGE   = 0x58,
       CLASS_CDO      = 0x118,          // UClass::ClassDefaultObject
       KEYEVENT_KEY   = 0x18,           // FKeyEvent::Key (an FKey: its FName first)
       // UWidgetCustomizationGrid::GetSelectedIndex is these four, (row + firstRow) * columns + column:
       GRID_COLUMNS = 0x268, GRID_FIRST_ROW = 0x364, GRID_COL = 0x370, GRID_ROW = 0x374,
       SHOPMENU_GRID  = 0x3e0,          // USkateShopMenuPageContainer::_gridWidget (UWidgetCustomizationGrid*)
       SHOPMENU_ACTOR = 0x3e8,          //   ..._characterCustomization (ACharacterCustomization*)
       CUSTOM_MODE    = 0x348,          // ACharacterCustomization::_currentMode: 0 what you own, 1 the shop's catalogue
       // The filtered list is rebuilt when any cached filter differs from the live one. The mode is NOT
       // one of them, so a change of mode has to spoil the cache by hand:
       CUSTOM_LIST_OK = 0x34c,          //   ..._cachedCharacterTypeFilter (live: +0x34d)
       CUSTOM_LIST_KEY = 0x354,         //   ..._cachedCategoryFilter, then _cachedBrandFilter +4 (live: +0x35c, +0x350)
       CUSTOM_ITEM_NONE = 0x258,        //   ..._customizationItemNone: the definition behind the "nothing" entry
       CUSTOM_BOARD   = 0x270,          //   ..._customizationSkateboard (ACustomizationSkateboard*): the board on show
       MENU_FLAGS     = 0x268,          // UMenuPageContainer: _pauseGame, _preventFocusChange, _navigationEnabled, _inputModePush
       MENU_FREEZED   = 0x292,          //   ..._inputsFreezed
       ACTOR_ROOT     = 0x130,          // AActor::RootComponent
       CHILD_ACTOR    = 0x200,          // UChildActorComponent::ChildActor
       COMP_C2W       = 0x1c0,          // USceneComponent::ComponentToWorld (FQuat, then translation +0x10)
       COMP_MOBILITY  = 0x14f,          // USceneComponent::Mobility: 0 static, 1 stationary, 2 movable
       CHAR_CAPSULE   = 0x290,          // ACharacter::CapsuleComponent
       CAPSULE_HALF_H = 0x468 };        // UCapsuleComponent::CapsuleHalfHeight

static const char* kShopClassPath = "/Game/SkateShop/PBP_SkateShop.PBP_SkateShop_C";

// void ASkateShop::HandleOnInteractionPromptConfirm() -- long, because its first 48 bytes are
// HideInteractionPrompt's too; the two part ways at the tail call into ShowSkateShopMenu.
static const char* SIG_SHOP_CONFIRM =
    "40 53 48 83 EC 20 48 8B D9 48 8B 89 28 03 00 00 48 85 C9 ?? ?? 48 81 C1 A0 02 00 00 48 8B D3 E8 ?? ?? ?? ?? "
    "48 8B 8B 28 03 00 00 E8 ?? ?? ?? ?? 48 8B 8B 28 03 00 00 E8 ?? ?? ?? ?? 48 C7 83 28 03 00 00 00 00 00 00 "
    "48 8B CB 48 83 C4 20 5B E9 ?? ?? ?? ??";
// AActor* UWorld::SpawnActor(UClass*, const FVector*, const FRotator*, const FActorSpawnParameters&),
// UWorld* AActor::GetWorld(), bool AActor::Destroy(bool, bool) -- the host's verified signatures.
static const char* SIG_SPAWN_ACTOR =
    "40 53 56 57 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 60 0F 28 1D ?? ?? ?? ?? 0F 57 D2 48 8B B4 24 B0 00 00 00 0F 28 CB";
static const char* SIG_ACTOR_WORLD =
    "40 53 48 83 EC 20 8B 41 08 48 8B D9 C1 E8 04 A8 01 75 76 48 8B 51 20 48 85 D2 74 6D 8B 42 08 C1 E8 0F A8 01 75 63 8B 42 0C 3B 05 ?? ?? ?? ??";
static const char* SIG_ACTOR_DESTROY =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 33 F6 41 0F B6 E8";
// void UWidget::SetKeyboardFocus(); void USceneComponent::SetWorldLocation(FVector, bool, FHitResult*,
// ETeleportType) -- the vector by pointer; and SetWorldRotation(const FQuat&, ...), the host's signature.
static const char* SIG_WIDGET_FOCUS =
    "40 53 48 83 EC 40 48 8D 54 24 20 48 8B D9 E8 ?? ?? ?? ?? 48 83 7C 24 20 00 ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 20 41 B0 02";
static const char* SIG_COMP_SET_LOC =
    "4C 8B DC 53 55 56 48 81 EC F0 00 00 00 41 0F 29 73 C8 45 0F 29 4B 98 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 90 00 00 00 F2 0F 10 02";
static const char* SIG_COMP_SET_ROT =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 41 0F B6 F8 49 8B D9 4C 8B C2 48 8B F1 48 8D 54 24 40 E8";
// bool UWidget::HasAnyUserFocus(). And InputHandler::PushInputMode, wanted only for where it keeps its
// count: its fourth instruction is `movsxd rdi, [rip+disp]` reading InputHandler::_inputModeStack's
// ArrayNum, which is data and cannot be scanned for itself.
static const char* SIG_WIDGET_HAS_FOCUS =
    "48 89 5C 24 08 57 48 83 EC 30 48 8D 54 24 20 E8 ?? ?? ?? ?? 48 8B 4C 24 20 48 85 C9 ?? ?? 48 8D 54 24 48 E8 ?? ?? ?? ??";
static const char* SIG_PUSH_INPUT_MODE =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 63 3D ?? ?? ?? ?? 48 8B DA 48 8B F1 8D 47 01 3B 05 ?? ?? ?? ?? 89 05 ?? ?? ?? ??";
typedef bool  (*WidgetHasFocusFn)(void* widget);
static WidgetHasFocusFn g_hasFocus = nullptr;
// void UWidgetCustomizationGrid::SetGridMode(mode): 1 the shop's, 0 an inventory's
static const char* SIG_GRID_SET_MODE =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8B EC 48 81 EC 80 00 00 00 8B F2 48 8B F9 85 D2 0F 85 ?? ?? ?? ??";
typedef void (*GridSetModeFn)(void* grid, int mode);
static GridSetModeFn g_gridMode = nullptr;
// FReply USkateShopMenuPageContainer::NativeOnKeyDown(const FGeometry&, const FKeyEvent&) -- the reply is
// a hidden return, so: (menu, reply*, geometry*, keyEvent*). Detoured for the closet's confirm.
static const char* SIG_SHOP_KEY_DOWN =
    "40 55 53 56 57 41 54 41 55 48 8D AC 24 18 FC FF FF 48 81 EC E8 04 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 B0 03 00 00 45 33 ED 49 8B D9 4D 8B E0";
// FSkateFilteredItem* ACharacterCustomization::GetFilteredCustomizationItem(int index),
// void ACharacterCustomization::SetProfileItem(FSkateFilteredItem*, bool),
// void UWidgetCustomizationGrid::SetSelected_Checked(bool)
static const char* SIG_GET_FILTERED_ITEM =
    "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 33 D2 E8 ?? ?? ?? ?? 85 DB ?? ?? 3B 9F 90 03 00 00 ?? ?? 85 DB ?? ?? 33 DB";
static const char* SIG_SET_PROFILE_ITEM =
    "4C 8B DC 55 56 57 41 54 41 55 41 57 49 8D AB F8 FE FF FF 48 81 EC D8 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 B8 00 00 00 4C 8B 3A";
static const char* SIG_GRID_SET_CHECKED =
    "48 89 5C 24 08 57 48 83 EC 20 0F B6 FA 48 8B D9 E8 ?? ?? ?? ?? 8B 93 74 03 00 00 03 93 64 03 00 00 0F AF 93 68 02 00 00";
typedef void* (*ShopKeyDownFn)(void* menu, void* replyOut, const void* geometry, const void* keyEvent);
typedef void* (*GetFilteredItemFn)(void* actor, int index);
typedef void  (*SetProfileItemFn)(void* actor, void* item, bool apply);
typedef void  (*GridSetCheckedFn)(void* grid, bool checked);
// void USkateShopMenuPageContainer::OpenWheelsOrientationWidget()
static const char* SIG_OPEN_WHEELS =
    "40 53 48 83 EC 20 48 83 B9 98 03 00 00 00 48 8B D9 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 93 98 03 00 00";
typedef void (*MenuVoidFn)(void* menu);
// void USkateShopMenuPageContainer::CharacterCustomization_UpdateDetails() -- the hover handler, detoured;
// void ACharacterCustomization::SampleCustomizationItem(const FCharacterCustomizationFilteredItem&,
//     bool keepItemAttributes, bool, TDelegate<void()>) -- the preview; the delegate goes by pointer and
//     both of the game's callers pass an unbound one (a null pointer and a zero size);
// void ASkateShop::ActivateSkateboardGearCamera() -- view target to the gear camera, blended. Its twin
//     for the DIY camera differs only in the component offset, which the signature runs through.
static const char* SIG_SHOP_UPDATE_DETAILS =
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 D0 FE FF FF 48 81 EC 30 02 00 00 "
    "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 20 01 00 00 4C 8B F1 48 8B 89 E0 03 00 00";
static const char* SIG_SAMPLE_ITEM =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 30 48 8B 81 30 02 00 00 45 0F B6 F1 "
    "44 8B 99 5C 03 00 00 41 0F B6 E8 48 8B F2 48 8B F9 48 8B 98 E8 00 00 00";
static const char* SIG_SHOP_GEAR_CAMERA =
    "48 89 5C 24 08 57 48 83 EC 40 48 8B 81 20 03 00 00 48 8B F9 48 8B 98 58 02 00 00 48 85 DB ?? ?? E8 ?? ?? ?? ?? "
    "48 8B 53 10 4C 8D 40 30 48 63 40 38 3B 42 38 ?? ?? 48 8B C8 48 8B 42 30 4C 39 04 C8 ?? ?? 33 DB 8B 44 24 3C "
    "4C 8D 44 24 20 48 8B 97 58 02 00 00 83 E0 FE F3 0F 10 87 84 02 00 00";
struct UnboundDelegate { void* instance; int32_t size; int32_t pad; };
typedef void (*SampleItemFn)(void* actor, void* item, bool keepItemAttributes, bool second, UnboundDelegate* onDone);
static MenuVoidFn        g_origDetails = nullptr;
static SampleItemFn      g_sampleItem  = nullptr;
static MenuVoidFn        g_openWheels  = nullptr;
static ShopKeyDownFn     g_origShopKey = nullptr;
static GetFilteredItemFn g_getItem     = nullptr;
static SetProfileItemFn  g_wearItem    = nullptr;
static GridSetCheckedFn  g_gridChecked = nullptr;
static const int32_t*   g_modeDepth = nullptr;     // how many input modes are stacked right now
typedef void  (*WidgetFocusFn)(void* widget);
typedef void  (*CompSetLocFn)(void* comp, const float* loc3, bool sweep, void* hit, int teleport);
typedef void  (*CompSetRotFn)(void* comp, const float* quat4, bool sweep, void* hit, unsigned char teleport);
static WidgetFocusFn g_focus  = nullptr;
static CompSetLocFn  g_setLoc = nullptr;
static CompSetRotFn  g_setRot = nullptr;
// void USceneComponent::SetMobility(EComponentMobility::Type) -- called on a plain scene component only.
static const char* SIG_COMP_SET_MOBILITY =
    "48 89 74 24 18 48 89 7C 24 20 55 48 8B EC 48 83 EC 50 0F B6 81 4F 01 00 00 8B F2 48 8B F9 3B D0 0F 84 ?? ?? ?? ?? "
    "48 89 5C 24 60 48 8D 4D F0 4C 89 74 24 68";
typedef void (*CompSetMobilityFn)(void* comp, int mobility);
static CompSetMobilityFn g_setMobility = nullptr;
typedef void  (*ShopConfirmFn)(void* shop);
typedef void* (*SpawnActorFn)(void* world, void* cls, const float* loc, const float* rot, void* params);
typedef void* (*ActorWorldFn)(void* actor);
typedef bool  (*ActorDestroyFn)(void* actor, bool netForce, bool shouldModifyLevel);
static ShopConfirmFn  g_shopConfirm = nullptr;
static SpawnActorFn   g_spawn       = nullptr;
static ActorWorldFn   g_worldOf     = nullptr;
static ActorDestroyFn g_destroy     = nullptr;
// void AActor::SetActorEnableCollision(bool) -- the host's signature -- and
// void ASkateShop::HideInteractionPrompt(): as long as it is because its first 48 bytes are the confirm
// handler's too, and the two part ways only at the end (this one returns, that one tail-calls).
static const char* SIG_ACTOR_COLLISION =
    "4C 8B DC 55 48 81 EC 00 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 F0 00 00 00 48 8B E9 0F B6 49 5C 0F B6 C1 C0 E8 03 24 01 3A C2 0F 84 ?? ?? ?? ??";
static const char* SIG_SHOP_HIDE_PROMPT =
    "40 53 48 83 EC 20 48 8B D9 48 8B 89 28 03 00 00 48 85 C9 ?? ?? 48 81 C1 A0 02 00 00 48 8B D3 E8 ?? ?? ?? ?? "
    "48 8B 8B 28 03 00 00 E8 ?? ?? ?? ?? 48 8B 8B 28 03 00 00 E8 ?? ?? ?? ?? 48 C7 83 28 03 00 00 00 00 00 00 "
    "48 83 C4 20 5B C3";
typedef void (*ActorCollisionFn)(void* actor, bool enable);
static ActorCollisionFn g_actorCollision = nullptr;
static ShopConfirmFn    g_shopHidePrompt = nullptr;
static ShopConfirmFn    g_gearCamera     = nullptr;

// ------------------------------------------------------------------ the wheels
enum Act { ACT_NONE, ACT_EMOTES, ACT_BOARD, ACT_CLOSET, ACT_PROPS, ACT_EMOTE, ACT_PROP, ACT_PROP_OPEN };
struct Entry { const char* label; Act act; };
// Clockwise from the top.
static const Entry kRoot[]   = { { "Emotes", ACT_EMOTES }, { "Board", ACT_BOARD },
                                 { "Props", ACT_PROPS },   { "Closet", ACT_CLOSET } };
// The second wheel is whatever emote.cpp performs, in its order: an entry's place IS the emote's number.
static Entry g_emotes[12];
// ...and the third lists the PROPS THEMSELVES -- one entry each -- while the fourth is what the prop
// you picked can do this moment (radio.cpp builds that fresh: take it out / put it down / next song /
// volume ...), so it is asked for again every frame the wheel is up.
// A PAGE PER PROP, rather than every prop's options in one list: there is one prop today and there
// will be more, and a flat list would become a jumble the moment there are two.
static Entry g_propList[8];
static Entry g_props[8];
static int   g_propPick = 0;       // which prop's page level 3 is showing -- its place in the list above
static const Entry* Wheel(int level, int* n, const char** title) {
    if (level == 3) {
        int k = Radio_WheelCount(); if (k > 8) k = 8;
        for (int i = 0; i < k; i++) { g_props[i].label = Radio_WheelLabel(i); g_props[i].act = ACT_PROP; }
        *n = k; *title = Radio_PropLabel(g_propPick); return g_props;
    }
    if (level == 2) {
        int k = Radio_PropCount(); if (k > 8) k = 8;
        for (int i = 0; i < k; i++) { g_propList[i].label = Radio_PropLabel(i); g_propList[i].act = ACT_PROP_OPEN; }
        *n = k; *title = "Props"; return g_propList;
    }
    if (level == 1) {
        int k = Emote_Count(); if (k > 12) k = 12;
        for (int i = 0; i < k; i++) { g_emotes[i].label = Emote_Name(i); g_emotes[i].act = ACT_EMOTE; }
        *n = k; *title = "Emotes"; return g_emotes;
    }
    *n = (int)(sizeof(kRoot) / sizeof(kRoot[0])); *title = ""; return kRoot;
}

// ------------------------------------------------------------------ state (game thread throughout:
// the two input hooks and the pump all run on it)
static int      g_open = 0, g_level = 0, g_sel = -1;
static float    g_rx = 0.0f, g_ry = 0.0f;
static int      g_reqToggle = 0, g_reqConfirm = 0, g_reqBack = 0;
static volatile LONGLONG g_pumpMs = 0;              // when Radial_PumpFrame last ran (see Radial_OnInputKey)
static int      g_swallowUp[3] = { 0, 0, 0 };        // R3, A, B: the release that pairs with a press we took
static char     g_msg[64] = ""; static uint64_t g_msgUntil = 0;
static void*    g_forSkater = nullptr;
// The shop whose menu we opened: ours (spawned, to be destroyed) or the level's (borrowed, to be put
// back as it was). 0 idle, 1 waiting for its menu, 2 menu up, 3 closed and waiting out the camera.
static void*    g_shop = nullptr, *g_shopPrevCollider = nullptr;
static SitObjRef g_shopRef = { nullptr, 0, 0, nullptr };    // ...and the proof it is still that shop (see SitUI_Alive)
static bool     g_shopOurs = false, g_spawnBroken = false;
static int      g_shopPhase = 0; static uint64_t g_shopPhaseMs = 0;
static int      g_shopDepth = 0, g_refocused = 0; static uint64_t g_focusCheckMs = 0;
static bool     g_shopCloset = false;               // this opening is the closet: what you own, not the catalogue
static bool     g_shopBoard  = false;               // ...and it is the board's page: the gear camera, aimed
static float    g_shopFeet[3] = { 0, 0, 0 }, g_shopYaw = 0.0f;      // where our shop was laid out, and facing what
static float    g_camAt[3] = { 0, 0, 0 }; static bool g_camSet = false; static int64_t g_camQpc = 0;
static float    g_camRest[3] = { 0, 0, 0 }; static int g_camLogged = 0;
static uint64_t g_fnR3 = 0, g_fnA = 0, g_fnB = 0, g_fnRX = 0, g_fnRY = 0, g_fnRT = 0, g_fnRTb = 0, g_fnRB = 0;
static int      g_rtAxisSeen = 0, g_swallowRT = 0;  // the trigger's axis has reported at least once; the button-press of it that we took
static uint64_t g_fnNot[32]; static int g_fnNotN = 0;

static void Say(const char* text) { snprintf(g_msg, sizeof(g_msg), "%s", text); g_msgUntil = GetTickCount64() + 2200; }

static bool OnFoot(void* sk) {
    void* ai = FootPlace_AnimInstance();
    return sk && ai && twkB(ai, AN_ON_BOARD) == 0;
}
static bool ShopMenuUp() { return g_shopPhase == 1 || g_shopPhase == 2; }
// May the wheel open, or stay open? Off the board, standing (a seat has its own buttons), no editor
// owning the screen, and not while the shop it opened is still up.
static bool Allowed() {
    void* sk = CatchTweaks_Skater();
    if (!g_on || !sk || Twk_IsProxy(sk)) return false;
    // A SEAT IS NOT A REASON TO REFUSE. Sitting owns B, A, the d-pad, X and Y -- never the stick's click --
    // and while the wheel is open its keys are asked for here first, so nothing is taken from the seat.
    // An emote is performed on top of the seated pose; the closet and the board, which move the skater,
    // are what a seat refuses (see Take).
    if (!OnFoot(sk) || Sit_EditorOpen()) return false;
    return !ShopMenuUp();
}
static void Close(const char* why) {
    if (!g_open) return;
    g_open = 0; g_level = 0; g_sel = -1; g_msg[0] = 0;
    SitUI_HideNow();                    // the borrowed A/B bar goes now, not when sitting next pumps it
    TwkLog("[radial] closed (%s)", why);
}

// ------------------------------------------------------------------ keys
// 1 click, 2 A, 3 B, 4 right X, 5 right Y, 6 the right trigger's AXIS, 7 the right trigger as a button,
// 8 RB (the board tap), 0 not ours. A name is resolved once per FName: this runs for every input event,
// the sticks' axes included.
static int KeyKind(const void* key) {
    const uint64_t nm = *(const uint64_t*)key;
    if (!nm) return 0;
    if (nm == g_fnR3) return 1;
    if (nm == g_fnA)  return 2;
    if (nm == g_fnB)  return 3;
    if (nm == g_fnRX) return 4;
    if (nm == g_fnRY) return 5;
    if (nm == g_fnRT) return 6;
    if (nm == g_fnRTb) return 7;
    if (nm == g_fnRB)  return 8;
    for (int i = 0; i < g_fnNotN; i++) if (nm == g_fnNot[i]) return 0;
    char nb[96];
    if (!GrindPop_FNameToString(key, nb, sizeof(nb))) return 0;
    if (!strcmp(nb, "Gamepad_RightThumbstick"))   { g_fnR3 = nm; return 1; }
    if (!strcmp(nb, "Gamepad_FaceButton_Bottom")) { g_fnA  = nm; return 2; }
    if (!strcmp(nb, "Gamepad_FaceButton_Right"))  { g_fnB  = nm; return 3; }
    if (!strcmp(nb, "Gamepad_RightX"))            { g_fnRX = nm; return 4; }
    if (!strcmp(nb, "Gamepad_RightY"))            { g_fnRY = nm; return 5; }
    if (!strcmp(nb, "Gamepad_RightTriggerAxis"))  { g_fnRT = nm; return 6; }
    if (!strcmp(nb, "Gamepad_RightTrigger"))      { g_fnRTb = nm; return 7; }
    if (!strcmp(nb, "Gamepad_RightShoulder"))     { g_fnRB = nm; return 8; }
    if (g_fnNotN < (int)(sizeof(g_fnNot) / sizeof(g_fnNot[0]))) g_fnNot[g_fnNotN++] = nm;
    return 0;
}
// THE WHEEL CANNOT OUTLIVE ITS PUMP. Everything that closes it -- a new level, getting on the board, an
// editor opening -- is decided in Radial_PumpFrame, and that rides the SKATER'S InputHandler::Tick. This
// hook rides the player's input object, which exists everywhere. So in a level with no skater (the
// apartment), and in the replay editor and the object dropper, the pump stops and the hook does not: a
// wheel open at that moment stayed "open" for good, swallowing A, B and the click -- field report: nothing
// in the apartment could be selected or backed out of, while both sticks still worked. (A first fix,
// making the pump see the skater go, could not work: the pump is what is missing. Log: not one line of
// any module after the switch.) So the hook itself lets go of a wheel nobody has serviced for half a
// second, takes what it drew off the screen, and lets the key through to whoever owns the screen now.
static bool Unserviced() {
    const LONGLONG last = g_pumpMs;
    return last && (LONGLONG)GetTickCount64() - last > 500;
}
static void Abandon() {
    const LONGLONG quiet = (LONGLONG)GetTickCount64() - g_pumpMs;
    g_open = 0; g_level = 0; g_sel = -1; g_msg[0] = 0;
    g_swallowUp[0] = g_swallowUp[1] = g_swallowUp[2] = 0; g_reqToggle = g_reqConfirm = g_reqBack = 0;
    SitUI_HideNow(); SitUI_HideFreeNow();
    TwkLog("[radial] closed (not serviced for %lld ms: a level without a skater, or an editor -- the keys are the game's again)", quiet);
}
int Radial_OnInputKey(const void* key, int ev, float* amount) {
    if (!g_on || !key) return 0;
    int kind = 0;
    __try { kind = KeyKind(key); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!kind) return 0;
    if ((g_open || g_swallowUp[0] || g_swallowUp[1] || g_swallowUp[2]) && Unserviced()) { Abandon(); return 0; }
    if (kind == 6) {                                        // the right trigger, as far as it is pulled: a Rage throws on it
        if (amount) { Emote_Trigger(*amount); g_rtAxisSeen = 1; }   // ALWAYS fed: a Rage must know one already held as it begins
        if (!Emote_WantsTrigger()) return 0;
        if (amount) *amount = 0.0f;                         // ...and while it is aiming the game does not get it
        return 2;
    }
    if (kind == 8) {                                        // RB: the board tap's pose, and its stick while held
        if (ev != 0 && ev != 1) return 0;
        const bool down = (ev == 0);
        if (!g_open) Emote_TapButton(down);
        // SWALLOWED ONLY WHILE IT IS OURS. RB is the game's own button otherwise, and taking it when
        // no board is up would break whatever it normally does.
        return (!g_open && Emote_TapHeld()) ? 1 : 0;
    }
    if (kind == 7) {                                        // ...and the same trigger as a BUTTON (the pad says "pressed" past 0.12)
        if (ev == 1) { if (!g_rtAxisSeen) Emote_Trigger(0.0f); if (!g_swallowRT) return 0; g_swallowRT = 0; return 1; }
        if (ev != 0 || !Emote_WantsTrigger()) return 0;
        if (!g_rtAxisSeen) Emote_Trigger(1.0f);             // a pad that never reports the axis: a press is a full pull
        g_swallowRT = 1;
        return 1;
    }
    if (kind >= 4) {                                        // the camera's stick: the wheel's, or an emote's (the board tap)
        if (!g_open && !Emote_WantsStick()) return 0;
        if (amount) *amount = 0.0f;
        return 2;
    }
    const int b = kind - 1;                                 // 0 click, 1 A, 2 B
    if (ev == 1) {                                          // a release pairs with the press we took
        if (!g_swallowUp[b]) return 0;
        g_swallowUp[b] = 0;
        return 1;
    }
    if (ev != 0) return g_open ? 1 : 0;                     // repeats while open are ours to ignore
    if (kind == 1) {
        if (!g_open && !Allowed()) return 0;                // on the board the click is the game's
        g_reqToggle = 1; g_swallowUp[0] = 1;
        return 1;
    }
    if (!g_open) {
        // B PUTS AN EMOTE AWAY, and that press does nothing else: every emote is held until then, and B is also the
        // seat's key (sit down; seated, the next position). Asked first here, so the seat never sees this press --
        // seated in the middle of a wave, B ends the wave, and only the NEXT B changes how you sit.
        if (kind == 3 && Radio_Holding()) { Radio_RequestPutDown(); g_swallowUp[b] = 1; return 1; }      // a radio under the arm: B puts it down
        if (kind == 3 && Emote_Stoppable()) { Emote_Stop(); g_swallowUp[b] = 1; return 1; }
        return 0;
    }
    if (kind == 2) g_reqConfirm = 1; else g_reqBack = 1;
    g_swallowUp[b] = 1;
    return 1;
}
void Radial_TickSticks(float* s) {
    if (!s) return;
    __try {
        g_rx = s[2]; g_ry = s[3];
        const bool emote = !g_open && Emote_WantsStick();   // the tap only while RB is HELD; a Rage never
        Emote_Stick(emote ? g_rx : 0.0f, emote ? g_ry : 0.0f);
        if (g_open || emote) { s[2] = 0.0f; s[3] = 0.0f; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// ------------------------------------------------------------------ what an entry does
// Where the skater stands: feet on the floor, and which way they face (degrees of yaw).
static bool Footing(void* sk, float loc[3], float* yawDeg) {
    __try {
        const uint8_t* root = *(const uint8_t* const*)((const uint8_t*)sk + ACTOR_ROOT);
        if (!root) return false;
        const float* q = (const float*)(root + COMP_C2W);           // x, y, z, w
        const float* t = (const float*)(root + COMP_C2W + 0x10);
        float half = 90.0f;
        const uint8_t* cap = *(const uint8_t* const*)((const uint8_t*)sk + CHAR_CAPSULE);
        if (cap) { const float h = *(const float*)(cap + CAPSULE_HALF_H); if (h > 20.0f && h < 200.0f) half = h; }
        loc[0] = t[0]; loc[1] = t[1]; loc[2] = t[2] - half;
        *yawDeg = atan2f(2.0f * (q[3] * q[2] + q[0] * q[1]), 1.0f - 2.0f * (q[1] * q[1] + q[2] * q[2])) * 57.29578f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void* SpawnShopAt(void* sk, void* cls, const float* loc, const float* rot) {
    uint8_t params[0x80]; memset(params, 0, sizeof(params));
    // A caught fault is not a recovery: whatever was half-built is abandoned in the world. So it is
    // loud and final -- never spawn again this run; the level's own shop is the fallback from then on.
    __try { void* world = g_worldOf(sk); return world ? g_spawn(world, cls, loc, rot, params) : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_spawnBroken = true;
                                           TwkLog("[radial] store: spawning a shop FAULTED -- not tried again this run");
                                           return nullptr; }
}
// A component's place relative to its actor, for the log: what the blueprint ships and what a level's
// designers made of it are the two things the layout below is a stand-in for.
static void LogLayout(const char* whose, void* shop) {
    static const struct { int off; const char* name; } kParts[] = {
        { SHOP_SKATER_ROOT, "skater" }, { SHOP_BOARD_ROOT, "board" }, { SHOP_CAMERA, "camera" },
        { SHOP_CAM_APPAREL, "apparelCam" }, { SHOP_CAM_GEAR, "gearCam" }, { SHOP_CAM_DIY, "diyCam" }, { SHOP_OBJ_ROOT, "objects" } };
    __try {
        char line[600]; int at = snprintf(line, sizeof(line), "[radial] layout of %s shop:", whose);
        for (const auto& k : kParts) {
            const uint8_t* c = *(const uint8_t* const*)((const uint8_t*)shop + k.off);
            if (!c) continue;
            const float* l = (const float*)(c + COMP_REL_LOC);
            at += snprintf(line + at, sizeof(line) - (size_t)at, " %s (%.0f %.0f %.0f | p%.0f y%.0f r%.0f)", k.name, l[0], l[1], l[2], l[3], l[4], l[5]);
            if (at > 520) break;
        }
        TwkLog("%s", line);
        const float* a = (const float*)((const uint8_t*)shop + SHOP_APPAREL_BASE);
        const float* b = (const float*)((const uint8_t*)shop + SHOP_BOARD_BASE);
        TwkLog("[radial]   apparel camera base (%.0f %.0f %.0f), board base (%.0f %.0f %.0f) -- world", a[0], a[1], a[2], b[0], b[1], b[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static void Place(void* comp, const float feet[3], const float f[3], const float r[3], float fwd, float right, float up,
                  float yawDeg, float pitchDeg) {
    if (!comp || !g_setLoc || !g_setRot) return;
    const float loc[3] = { feet[0] + f[0] * fwd + r[0] * right, feet[1] + f[1] * fwd + r[1] * right, feet[2] + up };
    const float hp = pitchDeg * 0.00872665f, hy = yawDeg * 0.00872665f;          // half angles, in radians
    const float q[4] = { sinf(hp) * sinf(hy), -sinf(hp) * cosf(hy), cosf(hp) * sinf(hy), cosf(hp) * cosf(hy) };
    __try { g_setLoc(comp, loc, false, nullptr, 1 /* teleport */); g_setRot(comp, q, false, nullptr, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}
// The shop, laid out around a skater standing at `feet` and facing `yaw`: the preview stands where you
// do, the cameras stand in front looking back, the board and the DIY objects get a spot to each side.
static void LayOut(void* shop, const float feet[3], float yaw) {
    const float yr = yaw * 0.0174533f;
    const float f[3] = { cosf(yr), sinf(yr), 0.0f }, r[3] = { -sinf(yr), cosf(yr), 0.0f };
    __try {
        const uint8_t* s = (const uint8_t*)shop;
        auto part = [&](int off) { return *(void* const*)(s + off); };
        Place(part(SHOP_SKATER_ROOT), feet, f, r,   0.0f,    0.0f,   0.0f, yaw,          0.0f);
        if (g_shopBoard) {
            // Made movable, then put to your right and turned a quarter left (see the header).
            uint8_t* root = (uint8_t*)part(SHOP_BOARD_ROOT);
            const int was = root ? root[COMP_MOBILITY] : -1;
            if (root && was != 2 && g_setMobility) g_setMobility(root, 2);
            Place(root, feet, f, r, g_boardRootFwd, g_boardRootRight, g_boardRootUp, yaw - 90.0f, 0.0f);
            if (root) {
                const float* t = (const float*)(root + COMP_C2W + 0x10);
                const float d[3] = { t[0] - feet[0], t[1] - feet[1], t[2] - feet[2] };
                TwkLog("[radial] board: the anchor is %.0f ahead, %.0f right, %.0f up of you, turned a quarter left (mobility %d -> %d%s)",
                       d[0] * f[0] + d[1] * f[1], d[0] * r[0] + d[1] * r[1], d[2], was, root[COMP_MOBILITY],
                       g_setMobility ? "" : "; NO SetMobility in this build");
            }
        }
        Place(part(SHOP_CAMERA),      feet, f, r, 340.0f,    0.0f, 120.0f, yaw + 180.0f, -5.0f);
        Place(part(SHOP_CAM_APPAREL), feet, f, r, 270.0f,    0.0f, 115.0f, yaw + 180.0f, -4.0f);
        Place(part(SHOP_CAM_GEAR),    feet, f, r, 200.0f,  110.0f,  90.0f, yaw + 180.0f, -6.0f);
        Place(part(SHOP_OBJ_ROOT),    feet, f, r,   0.0f, -300.0f,  60.0f, yaw,          0.0f);
        Place(part(SHOP_CAM_DIY),     feet, f, r, 450.0f, -300.0f, 110.0f, yaw + 180.0f, -5.0f);
        // ...and what BeginPlay recorded of the old layout, which the shop's tick steers back to.
        float* a = (float*)((uint8_t*)shop + SHOP_APPAREL_BASE);
        a[0] = feet[0] + f[0] * 270.0f; a[1] = feet[1] + f[1] * 270.0f; a[2] = feet[2] + 115.0f;
        float* b = (float*)((uint8_t*)shop + SHOP_BOARD_BASE);
        b[0] = feet[0] + f[0] * 40.0f + r[0] * 110.0f; b[1] = feet[1] + f[1] * 40.0f + r[1] * 110.0f; b[2] = feet[2] + 70.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) { TwkLog("[radial] store: laying the shop out faulted"); }
}
static void* SpawnShop(void* sk) {
    if (g_spawnBroken || !g_spawn || !g_worldOf) return nullptr;
    void* cls = SitUI_LoadObject(kShopClassPath);
    if (!cls) { TwkLog("[radial] store: %s did not load", kShopClassPath); return nullptr; }
    float loc[3], yaw = 0.0f;
    if (!Footing(sk, loc, &yaw)) return nullptr;
    const float rot[3] = { 0.0f, yaw, 0.0f };                       // FRotator: pitch, yaw, roll
    void* shop = SpawnShopAt(sk, cls, loc, rot);
    if (shop) {
        TwkLog("[radial] store: spawned a shop %p at (%.0f, %.0f, %.0f) yaw %.0f", shop, loc[0], loc[1], loc[2], yaw);
        // No trigger, from the first moment: spawned on top of you it has already seen you, and ending
        // that overlap is what makes it forget you and take its prompt down.
        __try { if (g_actorCollision) g_actorCollision(shop, false); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        LogLayout("the blueprint's", shop);
        LayOut(shop, loc, yaw);
        g_shopFeet[0] = loc[0]; g_shopFeet[1] = loc[1]; g_shopFeet[2] = loc[2]; g_shopYaw = yaw;
    }
    return shop;
}
// The Board menu's camera, kept in front of wherever the board on show really is (see the header). Eased
// rather than set: the board travels as each category opens, and the view should follow it, not cut.
// Never lower than knee height above where you stood -- the board starts out lying at your feet, and a
// camera at ground level a stride in front of you is under the ground on any slope.
static void AimBoardCamera() {
    __try {
        const uint8_t* shop  = (const uint8_t*)g_shop;
        const uint8_t* menu  = *(const uint8_t* const*)(shop + SHOP_MENU);
        const uint8_t* actor = menu  ? *(const uint8_t* const*)(menu + SHOPMENU_ACTOR) : nullptr;
        const uint8_t* board = actor ? *(const uint8_t* const*)(actor + CUSTOM_BOARD)  : nullptr;
        const uint8_t* root  = board ? *(const uint8_t* const*)(board + ACTOR_ROOT)    : nullptr;
        void* cam = *(void* const*)(shop + SHOP_CAM_GEAR);
        if (!root || !cam) return;
        const float* p = (const float*)(root + COMP_C2W + 0x10);
        if (!(p[0] == p[0]) || !(p[1] == p[1]) || !(p[2] == p[2])) return;
        LARGE_INTEGER t, fq; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&fq);
        float dt = g_camSet ? (float)((double)(t.QuadPart - g_camQpc) / (double)fq.QuadPart) : 0.0f;
        g_camQpc = t.QuadPart;
        if (dt > 0.1f) dt = 0.1f;
        const float k = g_camSet ? 1.0f - expf(-7.0f * dt) : 1.0f;
        float moved = 0.0f;
        for (int i = 0; i < 3; i++) { const float d = (p[i] - g_camAt[i]) * k; g_camAt[i] += d; moved += d * d; }
        const float yr = g_shopYaw * 0.0174533f;
        const float f[3] = { cosf(yr), sinf(yr), 0.0f }, r[3] = { -sinf(yr), cosf(yr), 0.0f };
        float up = g_boardCamUp;
        const float floorZ = g_shopFeet[2] + 35.0f;
        if (g_camAt[2] + up < floorZ) up = floorZ - g_camAt[2];
        const float pitch = -atan2f(up, g_boardCamDist > 1.0f ? g_boardCamDist : 1.0f) * 57.29578f;
        Place(cam, g_camAt, f, r, g_boardCamDist, g_boardCamSide, up, g_shopYaw + 180.0f, pitch);
        // Where the board comes to rest, from where you stood: the numbers a fixed layout would be built from.
        const float far2 = (p[0] - g_camRest[0]) * (p[0] - g_camRest[0]) + (p[1] - g_camRest[1]) * (p[1] - g_camRest[1])
                         + (p[2] - g_camRest[2]) * (p[2] - g_camRest[2]);
        if ((!g_camSet || (moved < 0.0004f && far2 > 25.0f)) && g_camLogged < 12) {
            g_camLogged++; g_camRest[0] = p[0]; g_camRest[1] = p[1]; g_camRest[2] = p[2];
            const float d[3] = { p[0] - g_shopFeet[0], p[1] - g_shopFeet[1], p[2] - g_shopFeet[2] };
            TwkLog("[radial] board: the board on show rests %.0f ahead, %.0f right, %.0f up of where you stood (category 0x%x)",
                   d[0] * f[0] + d[1] * f[1], d[0] * r[0] + d[1] * r[1], d[2], (unsigned)*(const uint16_t*)(menu + SHOPMENU_CATEGORY));
        }
        g_camSet = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_shopBoard = false; TwkLog("[radial] board: aiming the camera faulted -- left where it is"); }
}
static void ShopDone(const char* why) {
    if (g_shop) {
        __try {
            if (g_shopOurs) { if (g_destroy) g_destroy(g_shop, false, true); }
            else *(void**)((uint8_t*)g_shop + SHOP_COLLIDER) = g_shopPrevCollider;
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        TwkLog("[radial] store: %s (%s)", g_shopOurs ? "our shop destroyed" : "the level's shop put back as it was", why);
    }
    g_shop = nullptr; g_shopPrevCollider = nullptr; g_shopOurs = false; g_shopPhase = 0; g_shopBoard = false;
}
// Fill in what the confirm handler reads. Ours takes any of its three menu settings it is missing
// from the level's shop: a placed shop may have been given them by the level, not the blueprint.
static bool PrimeShop(void* shop, bool ours, void* level, void* sk) {
    __try {
        uint8_t* s = (uint8_t*)shop;
        static const int kCfg[3] = { SHOP_CUSTOM_BP, SHOP_MENU_BP, SHOP_ROOT_PAGE };
        for (int i = 0; i < 3 && ours && level; i++)
            if (!*(void**)(s + kCfg[i])) *(void**)(s + kCfg[i]) = *(void**)((uint8_t*)level + kCfg[i]);
        TwkLog("[radial] store: %s shop %p | customization %p, menu %p, root page %p | level shop %p", ours ? "our" : "the level's",
               shop, *(void**)(s + SHOP_CUSTOM_BP), *(void**)(s + SHOP_MENU_BP), *(void**)(s + SHOP_ROOT_PAGE), level);
        g_shopPrevCollider = *(void**)(s + SHOP_COLLIDER);
        *(void**)(s + SHOP_COLLIDER) = sk;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { TwkLog("[radial] store: the shop could not be read"); return false; }
}
// The store's menu, turned into the closet. Called once, as the menu appears: no category has been
// opened yet, so no list has been built from the catalogue.
static void MakeCloset(void* menu) {
    __try {
        uint8_t* actor = *(uint8_t**)((uint8_t*)menu + SHOPMENU_ACTOR);
        void* grid     = *(void**)((uint8_t*)menu + SHOPMENU_GRID);
        if (!actor) { TwkLog("[radial] closet: the menu has no customization actor -- left as the store"); return; }
        const int was = *(const int32_t*)(actor + CUSTOM_MODE);
        *(int32_t*)(actor + CUSTOM_MODE) = 0;
        *(int64_t*)(actor + CUSTOM_LIST_KEY) = -1;
        *(actor + CUSTOM_LIST_OK) = 0;
        if (grid && g_gridMode) g_gridMode(grid, 0);
        TwkLog("[radial] closet: customization mode %d -> 0 (what you own), list marked dirty, grid %s", was,
               grid && g_gridMode ? "set to inventory" : "LEFT in shop mode");
    } __except (EXCEPTION_EXECUTE_HANDLER) { TwkLog("[radial] closet: switching the menu to owned items faulted"); }
}
// The page a menu starts on: the sub-page behind the item with this key on the menu blueprint's own
// root page. Null when it is not there -- and then the menu is not opened (see OpenWardrobe).
static void* LandingPage(void* shop, const char* wantKey) {
    __try {
        const uint8_t* cls = *(const uint8_t* const*)((const uint8_t*)shop + SHOP_MENU_BP);
        const uint8_t* cdo = cls ? *(const uint8_t* const*)(cls + CLASS_CDO) : nullptr;
        const uint8_t* root = cdo ? *(const uint8_t* const*)(cdo + MENU_ROOT_PAGE) : nullptr;
        if (!root) return nullptr;
        const uint8_t* items = *(const uint8_t* const*)(root + PAGE_ITEMS);
        const int n = *(const int32_t*)(root + PAGE_ITEMS + 8);
        for (int i = 0; items && i < n && i < 12; i++) {
            const uint8_t* it = items + (size_t)i * ITEM_SIZE;
            char key[64] = "";
            if (GrindPop_FNameToString(it, key, sizeof(key)) && !strcmp(key, wantKey)) return *(void* const*)(it + ITEM_SUBPAGE);
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// Is this key event the menu's confirm? The pad's bottom face button (the one key the shop's own
// confirm branch tests for), or Enter -- and, as there, never an auto-repeat.
static bool IsConfirmKey(const void* keyEvent) {
    static uint64_t yes[2] = { 0, 0 }, no[24]; static int nNo = 0;
    if (*((const uint8_t*)keyEvent + KEYEVENT_REPEAT)) return false;
    const uint64_t nm = *(const uint64_t*)((const uint8_t*)keyEvent + KEYEVENT_KEY);
    if (!nm) return false;
    if (nm == yes[0] || nm == yes[1] || nm == g_fnA) return true;
    for (int i = 0; i < nNo; i++) if (nm == no[i]) return false;
    char nb[96];
    if (!GrindPop_FNameToString((const uint8_t*)keyEvent + KEYEVENT_KEY, nb, sizeof(nb))) return false;
    if (!strcmp(nb, "Gamepad_FaceButton_Bottom")) { yes[0] = nm; return true; }
    if (!strcmp(nb, "Enter")) { yes[1] = nm; return true; }
    if (nNo < 24) no[nNo++] = nm;
    return false;
}
// The confirm: wear (or fit) the selected item. The last two steps of the shop's purchase, none of the
// first. Only while an item list is on screen with a real item selected; false = not ours, nothing done.
static bool WearSelected(uint8_t* menu) {
    uint8_t* grid = *(uint8_t**)(menu + SHOPMENU_GRID);
    void* actor   = *(void**)(menu + SHOPMENU_ACTOR);
    if (!grid || !actor || !g_getItem || !g_wearItem || grid[GRID_VISIBILITY] != 0) return false;
    const int idx = (*(const int32_t*)(grid + GRID_ROW) + *(const int32_t*)(grid + GRID_FIRST_ROW)) * *(const int32_t*)(grid + GRID_COLUMNS)
                  + *(const int32_t*)(grid + GRID_COL);
    if (idx < 0 || idx >= *(const int32_t*)(grid + GRID_ITEM_COUNT)) return false;
    void* item = g_getItem(actor, idx);
    if (!item) return false;
    g_wearItem(actor, item, false);
    if (g_gridChecked) g_gridChecked(grid, (*((const uint8_t*)item + ITEM_FLAGS8) & 1) != 0);
    const unsigned category = *(const uint16_t*)(menu + SHOPMENU_CATEGORY);
    TwkLog("[radial] wardrobe: item %d of category 0x%x put on", idx, category);
    if (category == 0x100 && g_openWheels) g_openWheels(menu);        // wheels: which way round, as at a shop
    return true;
}
static void* hkShopKeyDown(void* menu, void* reply, const void* geometry, const void* keyEvent) {
    // Ours only: the menu this module opened, while it is up. Everything else is the game's, untouched.
    if (g_shopPhase == 2 && g_shop && menu && keyEvent && SitUI_Alive(&g_shopRef)) {
        __try {
            if (menu == *(void**)((uint8_t*)g_shop + SHOP_MENU)) {
                if (IsConfirmKey(keyEvent)) WearSelected((uint8_t*)menu);
                // Nothing in these menus is for sale, whatever the menu worked out about the item: with
                // this clear the game's confirm branch answers Handled and opens no buy page.
                *((uint8_t*)menu + SHOPMENU_CAN_BUY) = 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { TwkLog("[radial] wardrobe: putting the item on faulted"); }
    }
    return g_origShopKey(menu, reply, geometry, keyEvent);
}
// The hover: the game's handler first -- it fills in the name, the brand, the count -- then, in these
// menus, the entry previewed again as the copy you own (see the header). Returns having done nothing
// for anything that is not a real, owned entry of an item list that is on screen.
static void PreviewOwned(uint8_t* menu) {
    uint8_t* grid  = *(uint8_t**)(menu + SHOPMENU_GRID);
    uint8_t* actor = *(uint8_t**)(menu + SHOPMENU_ACTOR);
    if (!grid || !actor || grid[GRID_VISIBILITY] != 0) return;
    const int idx = (*(const int32_t*)(grid + GRID_ROW) + *(const int32_t*)(grid + GRID_FIRST_ROW)) * *(const int32_t*)(grid + GRID_COLUMNS)
                  + *(const int32_t*)(grid + GRID_COL);
    if (idx < 0 || idx >= *(const int32_t*)(grid + GRID_ITEM_COUNT)) return;
    uint8_t* item = (uint8_t*)g_getItem(actor, idx);
    const void* def = item ? *(void* const*)item : nullptr;
    if (!def || def == *(void* const*)(actor + CUSTOM_ITEM_NONE)) return;
    UnboundDelegate none = { nullptr, 0, 0 };
    g_sampleItem(actor, item, true, false, &none);
}
static void hkShopDetails(void* menu) {
    g_origDetails(menu);
    if (g_shopPhase != 2 || !g_shop || !g_shopCloset || !menu || !g_sampleItem || !g_getItem || !SitUI_Alive(&g_shopRef)) return;
    __try {
        if (menu == *(void**)((uint8_t*)g_shop + SHOP_MENU)) PreviewOwned((uint8_t*)menu);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_sampleItem = nullptr;
        TwkLog("[radial] wardrobe: previewing the owned item faulted -- left to the shop's own preview from here on");
    }
}
// Closet or Board: the shop's menu on the page behind `pageKey`, showing what you own. Every way this
// could end up as a working shop front is refused rather than risked: no detour on the key handler, no
// shop of our own to give a starting page to, or no such page.
static void OpenWardrobe(void* sk, const char* pageKey, const char* what, bool board) {
    if (g_shopPhase) { Say("One moment"); return; }
    if (!g_origShopKey || !g_shopConfirm || !g_actorCollision) {
        TwkLog("[radial] %s: refused -- the key handler is not hooked or the shop's trigger cannot be switched off, so nothing can be bought by mistake", what);
                                            Say("Not available in this build"); return; }
    g_shopCloset = true; g_shopBoard = board;
    void* shop = SpawnShop(sk);
    if (!shop) { TwkLog("[radial] %s: no shop could be spawned", what); Say("Not available here"); return; }
    void* page = LandingPage(shop, pageKey);
    if (!page) {
        TwkLog("[radial] %s: the shop menu has no '%s' page -- not opened", what, pageKey);
        __try { if (g_destroy) g_destroy(shop, false, true); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        Say("Not available in this build"); return;
    }
    const bool ours = true;
    void* level = SitUI_FindInstanceOf("SkateShop", sk, shop);
    if (level) LogLayout("the level's", level);
    if (!PrimeShop(shop, ours, level, sk)) return;
    __try { *(void**)((uint8_t*)shop + SHOP_ROOT_PAGE) = page; } __except (EXCEPTION_EXECUTE_HANDLER) { }
    Close(what);
    // Held only if it can be told apart from whatever might one day stand at its address. A shop that
    // could not be watched would open as an ordinary buying shop the moment the pump let go of it.
    SitUI_Track(&g_shopRef, shop);
    if (!g_shopRef.obj) {
        TwkLog("[radial] %s: the spawned shop is not in the object table as itself -- not opened", what);
        __try { *(void**)((uint8_t*)shop + SHOP_COLLIDER) = nullptr; if (g_destroy) g_destroy(shop, false, true); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        Say("Not available in this build"); return;
    }
    g_shop = shop; g_shopOurs = ours; g_shopPhase = 1; g_shopPhaseMs = GetTickCount64();
    __try { g_shopConfirm(shop); }
    __except (EXCEPTION_EXECUTE_HANDLER) { TwkLog("[radial] store: the shop FAULTED while opening"); ShopDone("fault"); }
}
// The menu is built a moment after the call (the shop asks for the DLC state first), so its arrival
// and its leaving are both watched for here.
// Is the shop we are holding still that shop? A level change takes it with the level, and the skater
// can come back at the very address it had, so "a new skater" is not proof enough on its own.
static bool ShopAlive() {
    if (!g_shop) return false;
    if (SitUI_Alive(&g_shopRef)) return true;
    TwkLog("[radial] store: the shop is gone (its level went) -- forgotten, never touched");
    g_shop = nullptr; g_shopPrevCollider = nullptr; g_shopOurs = false; g_shopPhase = 0; g_shopBoard = false; g_shopRef.obj = nullptr;
    return false;
}
static void PumpShop() {
    if (!g_shopPhase || !g_shop || !ShopAlive()) return;
    bool up = false;
    __try { up = *(void* const*)((const uint8_t*)g_shop + SHOP_MENU) != nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_shop = nullptr; g_shopPhase = 0; return; }
    const uint64_t now = GetTickCount64();
    if (g_shopPhase == 1) {
        if (up) {
            g_shopPhase = 2;
            __try {
                uint8_t* menu = *(uint8_t**)((uint8_t*)g_shop + SHOP_MENU);
                TwkLog("[radial] store: the menu is up | pause %d, keepsFocus %d, navigation %d, inputMode %d, frozen %d -> %s",
                       menu[MENU_FLAGS], menu[MENU_FLAGS + 1], menu[MENU_FLAGS + 2], menu[MENU_FLAGS + 3], menu[MENU_FREEZED],
                       g_focus ? "given keyboard focus" : "NO focus call in this build");
                if (g_focus) g_focus(menu);
                if (g_shopCloset) MakeCloset(menu);
                if (g_shopBoard) {
                    // The camera the shop's front page would have switched to. It reads the skater at the
                    // counter and the gear camera's actor without checking either.
                    const uint8_t* s = (const uint8_t*)g_shop;
                    const uint8_t* gear = *(const uint8_t* const*)(s + SHOP_CAM_GEAR);
                    g_camSet = false; g_camLogged = 0;
                    if (g_gearCamera && *(void* const*)(s + SHOP_COLLIDER) && gear && *(void* const*)(gear + CHILD_ACTOR)) {
                        AimBoardCamera();
                        g_gearCamera(g_shop);
                        TwkLog("[radial] board: the gear camera is on");
                    } else { g_shopBoard = false; TwkLog("[radial] board: no gear camera to switch to -- the overview stays"); }
                }
                g_shopDepth = g_modeDepth ? *g_modeDepth : 0;      // the shop's own entry is on it by now
                g_refocused = 0; g_focusCheckMs = now;
            } __except (EXCEPTION_EXECUTE_HANDLER) { TwkLog("[radial] store: focusing the menu faulted"); }
        }
        else if (now - g_shopPhaseMs > 15000) ShopDone("its menu never appeared");
    } else if (g_shopPhase == 2) {
        if (!up) {
            g_shopPhase = 3; g_shopPhaseMs = now;
            TwkLog("[radial] store: the menu closed (%d refocus)", g_refocused);
            // Nobody is at its counter any more, and it must not think otherwise while it waits to go.
            __try {
                uint8_t* s = (uint8_t*)g_shop;
                if (g_shopOurs) { *(void**)(s + SHOP_COLLIDER) = nullptr; *(s + SHOP_PROMPT_DUE) = 0;
                                  if (*(void**)(s + SHOP_PROMPT) && g_shopHidePrompt) g_shopHidePrompt(g_shop); }
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        else if (g_shopBoard) AimBoardCamera();
        if (up && g_focus && g_hasFocus && now - g_focusCheckMs >= 150) {
            g_focusCheckMs = now;
            __try {
                void* menu = *(void**)((uint8_t*)g_shop + SHOP_MENU);
                const int depth = g_modeDepth ? *g_modeDepth : g_shopDepth;
                if (menu && depth <= g_shopDepth && !g_hasFocus(menu)) {
                    g_focus(menu);
                    if (g_refocused++ < 8) TwkLog("[radial] store: the menu had lost focus -- given back (input modes %d, shop came up at %d)", depth, g_shopDepth);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { g_hasFocus = nullptr; TwkLog("[radial] store: the focus check faulted -- stopped"); }
        }
    } else if (up) {
        // A menu on a shop that was waiting to be destroyed. It must not be, under an open menu: take
        // it back as ours, which also shows it what you own rather than the catalogue.
        TwkLog("[radial] store: the shop opened a menu again while waiting to be removed -- kept, and treated as ours");
        g_shopPhase = 1; g_shopPhaseMs = now;
    } else {
        __try { uint8_t* s = (uint8_t*)g_shop;        // held inert for as long as it lasts
                if (g_shopOurs) { *(void**)(s + SHOP_COLLIDER) = nullptr; *(s + SHOP_PROMPT_DUE) = 0; } }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
        if (now - g_shopPhaseMs > 5000) ShopDone("closed");
    }
}
static void Take(const Entry& e, void* sk) {
    switch (e.act) {
    case ACT_EMOTES: g_level = 1; g_sel = -1; g_msg[0] = 0; break;
    // Both open a shop that stands the skater at its counter: not something to do to a seated body.
    case ACT_BOARD:  if (Sit_PoseHeld()) Say("Stand up first"); else OpenWardrobe(sk, "SkateShopBuySkateboardGear", "board", true); break;
    case ACT_CLOSET: if (Sit_PoseHeld()) Say("Stand up first"); else OpenWardrobe(sk, "SkateShopBuySkaterGear", "closet", false);   break;
    case ACT_PROPS:  if (Sit_PoseHeld()) Say("Stand up first"); else { g_level = 2; g_sel = -1; g_msg[0] = 0; } break;
    // A prop itself: open its own page of options. Which prop is its place in the list, the same way
    // ACT_PROP reads its place below.
    case ACT_PROP_OPEN: g_propPick = (int)(&e - g_propList); g_level = 3; g_sel = -1; g_msg[0] = 0; break;
    // A prop's own entry. Most close the wheel (you took it out, you put it down); the ones you may want again at
    // once -- next song, next station -- leave it up.
    case ACT_PROP: {
        bool keep = false;
        const char* label = e.label;
        if (!Radio_WheelTake((int)(&e - g_props), &keep)) Say(Radio_WhyNot());
        else if (!keep) Close(label);
        else { g_sel = -1; }
        break;
    }
    // The wheel gets out of the way and the emote plays: you are free to walk while it does.
    case ACT_EMOTE:  if (Emote_Play((int)(&e - g_emotes))) Close(e.label); else Say(Emote_WhyNot()); break;
    default: break;
    }
}

// ------------------------------------------------------------------ the pump
void Radial_PumpFrame() {
    g_pumpMs = (LONGLONG)GetTickCount64();
    void* sk = CatchTweaks_Skater();
    // A new skater is a new level: whatever shop there was went with the old one. Dropped, never touched.
    // ...and "no skater" counts: CatchTweaks_Skater answers null once the old one is gone, which is the
    // only sign a level WITHOUT a skater -- the apartment -- ever gives. A wheel left open across that
    // would keep its key hook swallowing A and B for good. The releases owed to presses taken in the old
    // level are forgotten too, so the first release in the new one is not eaten.
    if (sk != g_forSkater) { g_forSkater = sk; g_shop = nullptr; g_shopRef.obj = nullptr; g_shopPhase = 0; g_shopOurs = false; g_shopBoard = false;
                             g_swallowUp[0] = g_swallowUp[1] = g_swallowUp[2] = 0; g_reqToggle = g_reqConfirm = g_reqBack = 0;
                             if (g_open) Close(sk ? "new level" : "the skater is gone (a level change, or the apartment)"); }
    PumpShop();
    if (g_reqToggle) {
        g_reqToggle = 0;
        if (g_open) Close("click");
        else if (Allowed()) {
            // STRAIGHT TO THE SPEAKER when you are standing at one. Clicking the stick beside a radio
            // can only mean that radio, and making it the Props page saves the root wheel every time.
            // Back still steps out to the root, so nothing is lost.
            const bool atSpeaker = Radio_AtSpeaker() && !Sit_PoseHeld();
            if (atSpeaker) g_propPick = 0;                 // the radio's page: the speaker you are at
            g_open = 1; g_level = atSpeaker ? 3 : 0; g_sel = -1; g_msg[0] = 0;
            TwkLog("[radial] opened%s", atSpeaker ? " at the speaker" : "");
        }
    }
    if (g_open && !Allowed()) Close("no longer off the board");
    int n = 0; const char* title = "";
    const Entry* wheel = Wheel(g_level, &n, &title);
    if (g_open) {
        // Where the stick points, clockwise from up. Inside the deadzone the last pick stands, so the
        // stick can be let go before A is pressed.
        const float mag = sqrtf(g_rx * g_rx + g_ry * g_ry);
        if (mag > g_deadzone && n > 0) {
            float a = atan2f(g_rx, g_ry);                   // 0 = up, + = clockwise
            if (a < 0.0f) a += 6.2831853f;
            const float step = 6.2831853f / (float)n;
            g_sel = (int)((a + step * 0.5f) / step) % n;
        }
        if (g_reqBack) {
            g_reqBack = 0;
            // ONE PAGE AT A TIME: a prop's options step back to the props list, everything else to the
            // root. Jumping straight home from three levels in would lose your place.
            if (g_level > 0) { g_level = (g_level == 3) ? 2 : 0; g_sel = -1; g_msg[0] = 0; wheel = Wheel(g_level, &n, &title); }
            else Close("back");
        }
        if (g_reqConfirm) {
            g_reqConfirm = 0;
            if (g_open && g_sel >= 0 && g_sel < n) { Take(wheel[g_sel], sk); wheel = Wheel(g_level, &n, &title); }
        }
    }
    g_reqBack = 0; g_reqConfirm = 0;

    // ---- draw
    SitFreeLabel labels[12]; int nl = 0;
    float vw = 0.0f, vh = 0.0f;
    if (g_open && sk && SitUI_Viewport(sk, &vw, &vh)) {
        SitUI_SetFreeMetrics(g_charW, g_dim);
        const float cx = vw * 0.5f, cy = vh * 0.5f;
        const float r = (vw < vh ? vw : vh) * g_radiusPct * 0.01f;
        for (int i = 0; i < n && nl < 11; i++) {
            const float a = 6.2831853f * (float)i / (float)n;
            // The circle is widened sideways: words are long, and a true circle crowds left and right.
            labels[nl++] = { wheel[i].label, cx + sinf(a) * r * 1.35f, cy - cosf(a) * r, i == g_sel };
        }
        // The middle: a message while one is up, else what is lit, else which wheel this is.
        const bool msg = g_msg[0] && GetTickCount64() < g_msgUntil;
        const char* mid = msg ? g_msg : (g_sel >= 0 && g_sel < n) ? wheel[g_sel].label : title;
        if (mid && mid[0]) labels[nl++] = { mid, cx, cy, msg };
    }
    SitUI_PumpFree(sk, g_open != 0 && nl > 0, labels, nl);
    if (g_open) {
        SitPromptEntry bar[2] = { { "Select", 'A', 0.0f }, { g_level > 0 ? "Back" : "Close", 'B', 0.0f } };
        SitUI_PumpFrame(sk, true, bar, 2);
    }
}

// ------------------------------------------------------------------ config / menu / install
// ---- the actor kit, for a prop (radio.cpp): the same verified calls the spawned shop uses. GAME THREAD.
void* Radial_SpawnActor(void* anyActor, void* cls, const float loc[3], const float rotPYR[3]) {
    if (!g_spawn || !g_worldOf || !anyActor || !cls) return nullptr;
    uint8_t params[0x80]; memset(params, 0, sizeof(params));
    __try { void* world = g_worldOf(anyActor); return world ? g_spawn(world, cls, loc, rotPYR, params) : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void Radial_DestroyActor(void* actor) {
    if (!actor || !g_destroy) return;
    __try { g_destroy(actor, false, true); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
void Radial_PlaceComp(void* comp, const float loc[3], const float quat[4]) {
    if (!comp || !g_setLoc || !g_setRot) return;
    __try { g_setLoc(comp, loc, false, nullptr, 1 /* teleport */); g_setRot(comp, quat, false, nullptr, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}
void Radial_SetMovable(void* comp) {
    if (!comp || !g_setMobility) return;
    __try { g_setMobility(comp, 2); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
bool Radial_Open()    { return g_open != 0; }
bool Radial_Busy()    { return g_open != 0 || ShopMenuUp(); }
bool Radial_Enabled() { return g_on != 0; }
void Radial_SetEnabled(bool on) { g_on = on ? 1 : 0; if (!on) Close("turned off"); TwkMarkDirty(); }
void Radial_ResetDefaults() { g_on = 1; }
void Radial_ReadConfig(const char* buf) {
    g_on        = TwkIniInt(buf, "RadialEnabled", 1) ? 1 : 0;
    g_radiusPct = (float)TwkIniIntQuiet(buf, "RadialRadiusPct", 24);
    g_deadzone  = (float)TwkIniIntQuiet(buf, "RadialDeadzonePct", 45) * 0.01f;
    g_charW     = (float)TwkIniIntQuiet(buf, "RadialCharWx10", 160) * 0.1f;
    g_dim       = (float)TwkIniIntQuiet(buf, "RadialDimPct", 90) * 0.01f;
    g_boardCamDist = (float)TwkIniIntQuiet(buf, "RadialBoardCamDist", 120);
    g_boardCamUp   = (float)TwkIniIntQuiet(buf, "RadialBoardCamUp", 10);
    g_boardCamSide = (float)TwkIniIntQuiet(buf, "RadialBoardCamSide", 0);
    if (g_boardCamDist < 40.0f || g_boardCamDist > 400.0f) g_boardCamDist = 120.0f;
    if (g_boardCamUp < -50.0f || g_boardCamUp > 150.0f)    g_boardCamUp = 10.0f;
    if (g_boardCamSide < -150.0f || g_boardCamSide > 150.0f) g_boardCamSide = 0.0f;
    g_boardRootFwd   = (float)TwkIniIntQuiet(buf, "RadialBoardRootFwd", -10);
    g_boardRootRight = (float)TwkIniIntQuiet(buf, "RadialBoardRootRight", 75);
    g_boardRootUp    = (float)TwkIniIntQuiet(buf, "RadialBoardRootUp", 25);
    if (g_boardRootFwd < -200.0f || g_boardRootFwd > 200.0f)     g_boardRootFwd = -10.0f;
    if (g_boardRootRight < -300.0f || g_boardRootRight > 300.0f) g_boardRootRight = 75.0f;
    if (g_boardRootUp < -50.0f || g_boardRootUp > 150.0f)        g_boardRootUp = 25.0f;
    if (g_radiusPct < 8.0f || g_radiusPct > 45.0f) g_radiusPct = 24.0f;
    if (g_deadzone < 0.1f || g_deadzone > 0.95f)   g_deadzone = 0.45f;
}
void Radial_SaveConfig(char* buf, size_t cap) { TwkIniSetInt(buf, cap, "RadialEnabled", g_on); }
void Radial_DrawMenu(const OmpMenuApi* api) {
    bool on = g_on != 0;
    if (api->Checkbox("Radial menu (click the right stick while off the board)", &on)) Radial_SetEnabled(on);
    api->SameLine(); api->TextDisabled(g_shopConfirm ? "(A select, B back)" : "(closet and board unavailable this build)");
}
void Radial_Install() {
    g_shopConfirm = (ShopConfirmFn)TwkScanExe(SIG_SHOP_CONFIRM);
    g_spawn       = (SpawnActorFn)TwkScanExe(SIG_SPAWN_ACTOR);
    g_worldOf     = (ActorWorldFn)TwkScanExe(SIG_ACTOR_WORLD);
    g_destroy     = (ActorDestroyFn)TwkScanExe(SIG_ACTOR_DESTROY);
    g_actorCollision = (ActorCollisionFn)TwkScanExe(SIG_ACTOR_COLLISION);
    g_shopHidePrompt = (ShopConfirmFn)TwkScanExe(SIG_SHOP_HIDE_PROMPT);
    if (!g_actorCollision) TwkLog("[radial] the shop's trigger cannot be switched off in this build -- closet and board off");
    g_focus       = (WidgetFocusFn)TwkScanExe(SIG_WIDGET_FOCUS);
    g_hasFocus    = (WidgetHasFocusFn)TwkScanExe(SIG_WIDGET_HAS_FOCUS);
    g_gridMode    = (GridSetModeFn)TwkScanExe(SIG_GRID_SET_MODE);
    g_getItem     = (GetFilteredItemFn)TwkScanExe(SIG_GET_FILTERED_ITEM);
    g_wearItem    = (SetProfileItemFn)TwkScanExe(SIG_SET_PROFILE_ITEM);
    g_gridChecked = (GridSetCheckedFn)TwkScanExe(SIG_GRID_SET_CHECKED);
    g_openWheels  = (MenuVoidFn)TwkScanExe(SIG_OPEN_WHEELS);
    if (void* keyDown = TwkScanExe(SIG_SHOP_KEY_DOWN)) {
        if (!g_getItem || !g_wearItem) TwkLog("[radial] closet: the wear calls did not resolve -- closet off");
        else if (MH_CreateHook(keyDown, (void*)&hkShopKeyDown, (void**)&g_origShopKey) != MH_OK || MH_EnableHook(keyDown) != MH_OK) {
            g_origShopKey = nullptr; TwkLog("[radial] closet: hooking the shop menu's key handler failed -- closet off");
        }
    } else TwkLog("[radial] closet: the shop menu's key handler was not found -- closet off (game updated?)");
    // The preview of what you own, and the board's camera: each is a nicety, and without it the menus
    // still open and still wear things -- so a miss is logged and nothing is refused.
    g_sampleItem = (SampleItemFn)TwkScanExe(SIG_SAMPLE_ITEM);
    g_gearCamera = (ShopConfirmFn)TwkScanExe(SIG_SHOP_GEAR_CAMERA);
    if (void* details = g_sampleItem && g_getItem ? TwkScanExe(SIG_SHOP_UPDATE_DETAILS) : nullptr) {
        if (MH_CreateHook(details, (void*)&hkShopDetails, (void**)&g_origDetails) != MH_OK || MH_EnableHook(details) != MH_OK) {
            g_origDetails = nullptr; TwkLog("[radial] closet: hooking the shop menu's hover failed -- the shop's own preview stays");
        }
    } else TwkLog("[radial] closet: the hover or the preview call was not found -- the shop's own preview stays");
    if (!g_gearCamera) TwkLog("[radial] board: the gear camera call was not found -- the overview camera stays");
    if (const uint8_t* push = TwkScanExe(SIG_PUSH_INPUT_MODE)) {
        // +0x0f: 48 63 3D <disp32>, and a rip-relative address counts from the next instruction (+0x16)
        __try { int32_t disp; memcpy(&disp, push + 0x12, 4); g_modeDepth = (const int32_t*)(push + 0x16 + disp); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_modeDepth = nullptr; }
    }
    g_setLoc      = (CompSetLocFn)TwkScanExe(SIG_COMP_SET_LOC);
    g_setRot      = (CompSetRotFn)TwkScanExe(SIG_COMP_SET_ROT);
    g_setMobility = (CompSetMobilityFn)TwkScanExe(SIG_COMP_SET_MOBILITY);
    if (!g_setMobility) TwkLog("[radial] board: SetMobility was not found -- the board's anchor stays at your feet, facing the side");
    TwkLog("[radial] armed: right-stick click off the board | store %s, its own shop %s, layout %s, menu focus %s, kept %s",
           g_shopConfirm ? "ok" : "MISSING (game updated?)", g_spawn && g_worldOf && g_destroy ? "ok" : "MISSING (the level's shop only)",
           g_setLoc && g_setRot ? "ok" : "MISSING", g_focus ? "ok" : "MISSING",
           g_hasFocus ? (g_modeDepth ? "ok" : "ok (sub-screens not told apart)") : "MISSING");
}
