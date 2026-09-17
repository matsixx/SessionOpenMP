/* =====================================================================================================
 * omp_mod_api.h -- the SessionOpenMP mod channel, API version 1.
 *
 * Lets a UE4SS C++ mod send its own messages to the other players in a SessionOpenMP session.
 * Guide: docs/modding-api.md in the SessionOpenMP repository.
 *
 * This header may be copied, modified and shipped with any mod, under any license.
 * ===================================================================================================== */
#ifndef OMP_MOD_API_H
#define OMP_MOD_API_H

#include <stdint.h>
#include <string.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "psapi.lib")
#endif

#define OMPMOD_API_VERSION  1
#define OMPMOD_EVERYONE     (-1)      /* Send to every player who has the channel */
#define OMPMOD_MAX_PAYLOAD  1000      /* bytes per message */

#ifdef __cplusplus
#define OMPMOD_INLINE inline
#else
#define OMPMOD_INLINE __inline
#endif

/* `data` is only valid during the call. */
typedef void (*OmpMod_OnMessage)(int player, const uint8_t* data, int len, void* user);
/* joined: 1 = a player with your channel appeared, 0 = they left. */
typedef void (*OmpMod_OnPlayer)(int player, int joined, void* user);

typedef struct OmpModApi {
    int   version;    /* 0 = not bound (OpenMP is not loaded) */
    int   (*Register)(const char* channel, OmpMod_OnMessage onMessage, OmpMod_OnPlayer onPlayer, void* user);
    void  (*Unregister)(int handle);
    int   (*Send)(int handle, int player, const uint8_t* data, int len, int reliable);
    int   (*InSession)(void);
    int   (*Players)(int handle, int* out, int cap);
    int   (*IsAuthority)(int handle);
    int   (*Authority)(int handle);
    int   (*PlayerName)(int player, char* out, int cap);
    int   (*PlayerId)(int player, char* out, int cap);
    int   (*LocalName)(char* out, int cap);
    int   (*LocalId)(char* out, int cap);
    void* (*PlayerActor)(int player);
    int   (*ActorPlayer)(void* actor);
} OmpModApi;

/* SessionOpenMP and SessionTweaks both load as main.dll, so OpenMP is found by its exports rather than
 * by module name. Returns the API version, or 0 (and a zeroed struct) if OpenMP is not loaded. */
#define OMPMOD_BIND_(field)                                                              \
    do {                                                                                 \
        FARPROC p_ = GetProcAddress(m_, "OmpMod_" #field);                               \
        if (!p_) { memset(api, 0, sizeof(*api)); return 0; }                             \
        memcpy(&api->field, &p_, sizeof(p_));                                            \
    } while (0)

static OMPMOD_INLINE int OmpMod_Bind(OmpModApi* api) {
    HMODULE mods_[1024];
    DWORD   need_ = 0, i_, n_;
    HMODULE m_ = NULL;
    FARPROC ver_ = NULL;
    memset(api, 0, sizeof(*api));
    if (!EnumProcessModules(GetCurrentProcess(), mods_, sizeof(mods_), &need_)) return 0;
    n_ = need_ / sizeof(HMODULE);
    if (n_ > 1024) n_ = 1024;
    for (i_ = 0; i_ < n_; i_++) {
        ver_ = GetProcAddress(mods_[i_], "OmpMod_ApiVersion");
        if (ver_) { m_ = mods_[i_]; break; }
    }
    if (!m_) return 0;
    OMPMOD_BIND_(Register);
    OMPMOD_BIND_(Unregister);
    OMPMOD_BIND_(Send);
    OMPMOD_BIND_(InSession);
    OMPMOD_BIND_(Players);
    OMPMOD_BIND_(IsAuthority);
    OMPMOD_BIND_(Authority);
    OMPMOD_BIND_(PlayerName);
    OMPMOD_BIND_(PlayerId);
    OMPMOD_BIND_(LocalName);
    OMPMOD_BIND_(LocalId);
    OMPMOD_BIND_(PlayerActor);
    OMPMOD_BIND_(ActorPlayer);
    api->version = ((int (*)(void))ver_)();
    return api->version;
}

#undef OMPMOD_BIND_

#endif /* OMP_MOD_API_H */
