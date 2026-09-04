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
// SessionTweaks -- RUN OUT instead of ragdolling when a trick is missed low to the ground:
// the game's own ASkaterCharacterBase::SetOnFootMode (skater on foot, board dropped into free
// physics -- the native "trick just fails" consequence). Big air still bails for real.
#pragma once
struct OmpMenuApi;
void RunOut_ReadConfig(const char* iniText);  // parse this module's keys + echo them
void RunOut_SaveConfig(char* iniText, size_t cap);  // write the live values back into the text
void RunOut_Install();                        // sig-scan + hook; per-symbol non-fatal
void RunOut_DrawMenu(const OmpMenuApi* api);  // RENDER THREAD (menu_ext contract)
// The accessors below back the pause-menu page and run on the GAME THREAD (the other menu_ext
// contract). Restore this module's shipped defaults:
void RunOut_ResetDefaults();
long RunOut_BailCalls();                      // Bail calls seen on our skater (any source)
void RunOut_BailNow(void* skater);            // bail this skater now (run-out where the policy allows)
bool RunOut_Enabled();
void RunOut_SetEnabled(bool on);
// In CENTIMETRES, not metres: the pause menu's slider row prints its value with %d, so a knob
// exposed there has to live in a unit whose integers are meaningful (0.5..6.0 m would only ever
// display 0..6). The F1 slider keeps metres -- it can format floats.
float RunOut_MaxDropCm();
void  RunOut_SetMaxDropCm(float cm);
void RunOut_PumpFrame();                      // game thread, once per input tick: the armed
                                              // post-conversion fall watch (impact bail)
