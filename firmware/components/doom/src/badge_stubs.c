// Stubs for the subsystems the badge physically cannot use.
//
// The board has no speaker, amp or DAC, so every sound and music entry point is
// a no-op here rather than a silent backend: this way the callers keep working
// untouched while the mixer, the MUS converter and their buffers are gone.
//
// Likewise there is no joystick, no DOS text console for ENDOOM, and no
// filesystem -- the WAD is mapped from flash -- so those backends are gone too.

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"
#include "i_joystick.h"
#include "sha1.h"
#include "w_checksum.h"

// ---------------------------------------------------------------- sound
void I_InitSound(boolean use_sfx_prefix)          { (void)use_sfx_prefix; }
void I_ShutdownSound(void)                        { }
int  I_GetSfxLumpNum(sfxinfo_t *s)                { (void)s; return 0; }
void I_UpdateSound(void)                          { }
void I_UpdateSoundParams(int c, int v, int s)     { (void)c; (void)v; (void)s; }
int  I_StartSound(sfxinfo_t *s, int c, int v, int sep)
                                                  { (void)s; (void)c; (void)v;
                                                    (void)sep; return -1; }
void I_StopSound(int channel)                     { (void)channel; }
boolean I_SoundIsPlaying(int channel)             { (void)channel; return false; }
void I_PrecacheSounds(sfxinfo_t *s, int n)        { (void)s; (void)n; }
void I_BindSoundVariables(void)                   { }
void I_InitTimidityConfig(void)                   { }

// ---------------------------------------------------------------- music
void  I_InitMusic(void)                           { }
void  I_ShutdownMusic(void)                       { }
void  I_SetMusicVolume(int volume)                { (void)volume; }
void  I_PauseSong(void)                           { }
void  I_ResumeSong(void)                          { }
void *I_RegisterSong(void *data, int len)         { (void)data; (void)len;
                                                    return NULL; }
void  I_UnRegisterSong(void *handle)              { (void)handle; }
void  I_PlaySong(void *handle, boolean looping)   { (void)handle; (void)looping; }
void  I_StopSong(void)                            { }
boolean I_MusicIsPlaying(void)                    { return false; }

// ---------------------------------------------------------------- joystick
void I_InitJoystick(void)                         { }
void I_ShutdownJoystick(void)                     { }
void I_UpdateJoystick(void)                       { }
void I_BindJoystickVariables(void)                { }

// ---------------------------------------------------------------- ENDOOM
// The DOS text screen shown on exit. There is no text console to show it on,
// and the lump is not even in our WAD.
void I_Endoom(byte *endoom_data)                  { (void)endoom_data; }

// ---------------------------------------------- demo/netgame checksums
// Only used to verify demo and netgame consistency, neither of which a
// single badge does.
void SHA1_Init(sha1_context_t *c)                 { (void)c; }
void SHA1_Update(sha1_context_t *c, byte *d, unsigned int l)
                                                  { (void)c; (void)d; (void)l; }
void SHA1_Final(sha1_digest_t digest, sha1_context_t *c)
                                                  { (void)digest; (void)c; }
void SHA1_UpdateInt32(sha1_context_t *c, unsigned int val)
                                                  { (void)c; (void)val; }
void SHA1_UpdateString(sha1_context_t *c, char *str)
                                                  { (void)c; (void)str; }

void W_Checksum(sha1_digest_t digest)             { memset(digest, 0, sizeof(sha1_digest_t)); }

// ---------------------------------------------------------------- statdump
// End-of-level statistics dumping, a debugging aid for demo verification.
void StatCopy(void *stats)                        { (void)stats; }
void StatDump(void)                               { }

// ------------------------------------------------- netgame globals
// Their definitions lived in the net client, which is gone. The engine still
// branches on them, and with both false every one of those branches takes the
// single-player path.
boolean drone = false;
boolean net_client_connected = false;

// Lived in i_sound.c. Nothing reads it now except S_ChangeMusic's guard.
int snd_musicdevice = 0;
