/*
 * audio.h - sound out through AHI, for the preview player.
 *
 * ahi.device on a unit (the AHIUnit setting, 0 by default), 16-bit stereo at FB_PLAY_RATE, two
 * requests of 100 ms queued back to back (each linked to the one before,
 * so there are no gaps). Feed() is called from the main loop: it collects
 * finished requests and sends new ones, filled by a callback (the
 * player's FB_PlayerTakeAudio).
 *
 * AmigaOS only (src/player/audio.c); not part of the host build.
 */
#ifndef FORMBROD_AUDIO_H
#define FORMBROD_AUDIO_H

typedef long (*AudioFill)(void *ud, short *out, long frames);

/* Opens AHI's `unit` (0 = the default; see AHI prefs). 0, or -1 if it
 * can't be opened (the preview then plays without sound; the log says
 * why). */
int  Audio_Open(int unit);
void Audio_Feed(AudioFill fill, void *ud);
/* Stops what's queued at once; Open again is not needed to go on. */
void Audio_Stop(void);
void Audio_Close(void);

#endif
