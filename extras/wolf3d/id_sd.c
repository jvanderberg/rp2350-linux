// SPDX-License-Identifier: GPL-2.0-only
//
// ID_SD.C - the sound manager, without sound.
//
// Replaces Wolf4SDL's id_sd.c (by Jason Blochowiak, ported to SDL by the
// Wolf4SDL authors), which plays AdLib music and effects through
// SDL_mixer. The board has no audio output, so this keeps the interface
// the rest of the engine calls and does nothing: every sound is "done"
// at once, and there is no music. Delay() is kept from the original.
//

#include "wl_def.h"

globalsoundpos channelSoundPos[MIX_CHANNELS];

boolean AdLibPresent, SoundBlasterPresent, SBProPresent, SoundPositioned;
byte    SoundMode, DigiMode, MusicMode;
word    NumDigi;
digiinfo *DigiList;
int     DigiMap[LASTSOUND];
int     DigiChannel[STARTMUSIC - STARTDIGISOUNDS];

void Delay (int32_t wolfticks)
{
    if (wolfticks > 0)
        SDL_Delay ((wolfticks * 100) / 7);
}

void SD_Startup (void)
{
    int i;

    for (i = 0; i < LASTSOUND; i++)
        DigiMap[i] = -1;
    SoundMode = sdm_Off;
    MusicMode = smm_Off;
    DigiMode = sds_Off;
}

void SD_Shutdown (void) {}
int SD_GetChannelForDigi (int which) { (void)which; return 0; }
void SD_PositionSound (int leftvol, int rightvol) { (void)leftvol; (void)rightvol; }
boolean SD_PlaySound (int sound) { (void)sound; return false; }
void SD_SetPosition (int channel, int leftvol, int rightvol) { (void)channel; (void)leftvol; (void)rightvol; }
void SD_StopSound (void) {}
void SD_WaitSoundDone (void) {}
void SD_StartMusic (int chunk) { (void)chunk; }
void SD_ContinueMusic (int chunk, int startoffs) { (void)chunk; (void)startoffs; }
void SD_MusicOn (void) {}
void SD_FadeOutMusic (void) {}
int SD_MusicOff (void) { return 0; }
boolean SD_MusicPlaying (void) { return false; }
boolean SD_SetSoundMode (byte mode) { SoundMode = sdm_Off; (void)mode; return true; }
boolean SD_SetMusicMode (byte mode) { MusicMode = smm_Off; (void)mode; return true; }
word SD_SoundPlaying (void) { return 0; }
void SD_SetDigiDevice (byte mode) { DigiMode = sds_Off; (void)mode; }
void SD_PrepareSound (int which) { (void)which; }
int SD_PlayDigitized (word which, int leftpos, int rightpos) { (void)which; (void)leftpos; (void)rightpos; return 0; }
void SD_StopDigitized (void) {}
