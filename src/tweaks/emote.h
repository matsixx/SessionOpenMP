// SessionTweaks -- EMOTES: gestures performed on the skater's own skeleton, off the board. See emote.cpp.
#pragma once

void Emote_ReadConfig(const char* iniText);
void Emote_PumpFrame();                  // GAME THREAD: the clock, the blend, and everything that ends an emote
void Emote_OnFlip(void* mesh);           // from sit's FlipEditableSpaceBases detour, AFTER the seat's own pose
void Emote_Watchdog();                   // from a tick that survives the editors: the pump does not (see sit.cpp)

int         Emote_Count();
const char* Emote_Name(int index);
bool        Emote_Play(int wheelIndex);  // a place ON THE WHEEL. false = refused, and the log says why
// RB: a press holds the board in the tap pose (the camera stays yours), and HOLDING it takes the right
// stick to work the tap. A second short press puts the board down. Fed by the radial's key hook.
void        Emote_TapButton(bool down);
bool        Emote_TapHeld();             // RB held: the stick is the tap's and the camera is pinned
const char* Emote_WhyNot();              // ...and so does this, in words for the wheel
void        Emote_Stop();
bool        Emote_Active();
bool        Emote_WantsStick();          // the right stick is an emote's just now (the board tap): the camera does not get it
void        Emote_Stick(float rx, float ry);    // ...and this is where it goes, every input tick
void        Emote_Trigger(float rt);            // the right trigger, 0..1, whenever the pad reports it (always fed)
bool        Emote_WantsTrigger();               // ...and it is an emote's just now (a Rage: it throws, as hard as it is pulled)
bool        Emote_Stoppable();                  // B would put an emote away: it does that and NOTHING else with this press
// A CARRIED PROP (radio.cpp): a box under the free arm, long ways. `origin`/`extent` = the mesh's bounds, its own space.
bool        Emote_CarryStart(const float origin[3], const float extent[3]);
void        Emote_CarryStop();
bool        Emote_Carrying();
unsigned long long Emote_ChestBoneOf(void* skeletalMeshComponent);      // any character's: the bone a carried prop hangs from
bool        Emote_CarryPlace(float posW[3], float quatW[4], float* weight, unsigned long long* chestBone);
struct SitPromptEntry;
int         Emote_Prompts(SitPromptEntry* out, int cap);    // what the prompt bar should say while one is held (0 = nothing)
bool        Emote_PoseHeld();            // the skeleton is posed outside the graph: it travels to other players
