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
// SessionTweaks -- FSR UPSCALING through the temporal-upscaler seam (see upscale_fsr.cpp).
#pragma once
#include <stdint.h>
void UpscaleFsr_ReadConfig(const char* iniText);
void UpscaleFsr_SaveConfig(char* iniText, size_t cap);
void UpscaleFsr_ResetDefaults();
void UpscaleFsr_Install();                  // resolves the engine's render-graph functions; non-fatal
void UpscaleFsr_PumpFrame();                // GAME THREAD: the 1/s status line
void UpscaleFsr_SetGameplay(bool inLevel);  // GAME THREAD: FSR runs only in a level, never in the menu
// RENDER THREAD, from the seam AFTER the engine's upscaler produced *outColor / outRect. Returns true
// when it replaced them with FSR's output.
bool UpscaleFsr_AddPasses(void* graphBuilder, const uint8_t* view, const uint8_t* passInputs, void** outColor, int32_t* outRect);
bool UpscaleFsr_Active();                   // on, engine resolved, not failed this session
int  UpscaleFsr_WantedTaaSamples();         // r.TemporalAASamples to hold while on / off
const char* UpscaleFsr_Status();
bool  UpscaleFsr_Enabled();      void UpscaleFsr_SetEnabled(bool on);
float UpscaleFsr_SharpnessPct(); void UpscaleFsr_SetSharpnessPct(float v);
bool  UpscaleFsr_PreferFsr4();   void UpscaleFsr_SetPreferFsr4(bool on);
