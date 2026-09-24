// SessionOpenMP -- players whose dropped objects YOU do not see. Yours alone, like the mute list: it
// is applied on the receiving end, so nothing about it reaches the hidden player or anyone else.
// A hidden player's objects are taken out of your world -- visual AND collision, you cannot ride
// them -- and their table is kept, so unhiding brings the lot straight back with no resync.
// A plain text file beside the log, one entry per line: `<productUserId> <name>`, the ban and mute
// lists' shape exactly, and for the same reason: it is your list, readable and editable by hand.
#pragma once

enum { OMP_OBJHIDE_MAX = 256 };

// `dir` is the folder holding SessionOpenMP.log. Loads the saved list; absence is normal.
void ObjHide_Init(const char* dir, void (*logf)(const char*));
bool ObjHide_Add(const char* peerId, const char* name);   // false = full
bool ObjHide_Is(const char* peerId);
bool ObjHide_Remove(const char* peerId);
int  ObjHide_Count();
