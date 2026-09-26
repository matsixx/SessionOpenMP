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
//
// THE CONTROLLER, SAMPLED ON ITS OWN CLOCK.
//
// The flip and scoop multipliers are built from how fast the thumb moved. The game only looks at the
// stick once a frame, so any measurement built from its samples inherits the frame: a flick that
// finishes inside one frame is seen as a single step, where the step falls relative to the flick
// changes run to run, and everything else the game thread does between the pad poll and the sample
// (the frame's other work, and in co-op the whole session update) lands in the timing. That is the
// "I flicked fast and it flicked slow", and why it can feel different in a lobby at the same frame
// rate.
//
// This module reads the pad itself, on a thread of its own at ~1 kHz, and keeps every change with a
// QueryPerformanceCounter timestamp taken the moment the read returned. The measurements below work
// on those samples, so a flick's timing is the thumb's timing: independent of frame rate, of frame
// pacing, and of anything the game thread is doing. The resolution left is the controller's own
// report interval (typically 1-8 ms), which the status line reports.
//
// The reads go AROUND pop_probe's XInput hooks (its trampolines), so the pop scheme's pad machine
// never sees them. No XInput pad = no samples, and the callers keep their per-frame measurement.
#pragma once
#include <cstddef>

struct OmpMenuApi;

void   PadSampler_ReadConfig(const char* iniText);
void   PadSampler_SaveConfig(char* iniText, size_t cap);
void   PadSampler_ResetDefaults();
void   PadSampler_Start();              // after PopProbe_Install (the reads use its trampolines)
void   PadSampler_Stop();
void   PadSampler_PumpFrame();          // GAME THREAD: the status line; time-throttled
bool   PadSampler_Enabled();
void   PadSampler_SetEnabled(bool on);
// A connected pad has been read within the last 100 ms.
bool   PadSampler_Live();
// The samples' clock (QPC, seconds) -- the same one scoop_speed's trackers use.
double PadSampler_Now();

// A FLICK: the most recent movement of one stick away from where it was resting. speed = 0.8 * D /
// (t90 - t10), where D is how far it travelled from the rest point and t10/t90 are the moments it
// passed 10% and 90% of that, interpolated between reports. Stick units per second; a flick that
// covers the stick in 40 ms reads ~25.
struct PadFlick {
    float speed = 0.0f;       // stick units / s over the 10-90% rise
    float dist = 0.0f;        // D, stick units
    float riseMs = 0.0f;      // t90 - t10
    float onsetAgoMs = 0.0f;  // now - t10: how long ago the flick got going
    int   samples = 0;        // reports inside the rise (the resolution actually available)
};
bool PadSampler_Flick(bool rightStick, float windowSec, PadFlick* out);

// A SCOOP: the current angular sweep of one stick (samples at or above minMag, the stick's run away
// from the centre), measured exactly like scoop_speed's tracker -- the fastest rate over pairs of
// samples a sustain baseline apart -- but on the controller's own reports.
struct PadSweep {
    float sustained = 0.0f;   // deg/s
    float sweep = 0.0f;       // deg, whole gesture
    float peak = 0.0f;        // deg/s over ~8 ms (logged only)
    int   samples = 0;
};
bool PadSampler_Sweep(bool rightStick, float minMag, float freshSec, float sustainFrac, PadSweep* out);

void PadSampler_DrawStatus(const OmpMenuApi* api);   // RENDER THREAD
