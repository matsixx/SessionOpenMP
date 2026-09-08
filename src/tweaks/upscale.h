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
// SessionTweaks -- UPSCALING: the render scale, the engine's temporal upsampler, and the
// temporal-upscaler seam an FSR upscaler will hang off. See upscale.cpp.
#pragma once
void Upscale_ReadConfig(const char* iniText);
void Upscale_SaveConfig(char* iniText, size_t cap);
void Upscale_ResetDefaults();
void Upscale_Install();                 // sig-scan + the seam hook; non-fatal if missing
void Upscale_PumpFrame();               // GAME THREAD: applies the console variables, 1 Hz FSR health line
// Menu accessors (menu_ext contract).
bool  Upscale_TaauEnabled();    void Upscale_SetTaauEnabled(bool on);
float Upscale_RenderScalePct(); void Upscale_SetRenderScalePct(float v);
