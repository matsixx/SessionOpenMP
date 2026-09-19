// SessionTweaks -- the radio: a speaker you carry under your arm and put down, playing the game's own music from
// where it stands. See radio.cpp. GAME THREAD throughout (the wheel and the pump both run on it); the one call
// made from the key hook only sets a flag.
#pragma once
#include <stddef.h>

void Radio_ReadConfig(const char* iniText);
void Radio_PumpFrame();
void Radio_SaveConfig(char* iniText, size_t cap);      // your own radio's volume, as the wheel last left it

// The Props wheel: what can be done with the radio right now, built fresh each time it is asked for.
int         Radio_WheelCount();
const char* Radio_WheelLabel(int i);
bool        Radio_WheelTake(int i, bool* keepWheelOpen);   // false = refused, and Radio_WhyNot says why
const char* Radio_WhyNot();

bool Radio_Holding();            // it is under your arm: B puts it down (asked by the radial's key hook, before "stop emote")
void Radio_RequestPutDown();     // from the key hook: acted on by the pump
