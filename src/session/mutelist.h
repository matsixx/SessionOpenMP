// SessionOpenMP -- the voice mute list: people whose voice YOU do not hear. Yours alone -- it is
// applied on the receiving end, so nothing about it reaches the muted player or anyone else.
// A plain text file beside the log, one entry per line: `<productUserId> <name>`, like the ban
// list and for the same reason: it is your own list, readable and editable by hand.
#pragma once

enum { OMP_MUTE_MAX = 256 };

// `dir` is the folder holding SessionOpenMP.log. Loads the saved list; absence is normal.
void Mute_Init(const char* dir, void (*logf)(const char*));
bool Mute_Add(const char* peerId, const char* name);   // false = full
bool Mute_Is(const char* peerId);
bool Mute_Remove(const char* peerId);
int  Mute_Count();
bool Mute_At(int i, char* idOut, int idCap, char* nameOut, int nameCap);
