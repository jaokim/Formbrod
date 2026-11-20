/*
 * playwindow.h - the preview player's window: the picture, and the time
 * under it.
 *
 * It opens as soon as playing starts (PlayWindow_Prepare), saying the
 * preview is being prepared until the first picture arrives, and stays
 * open after the preview stops, showing the last picture. Closing it, or
 * Esc, stops the preview (App_PlayStop).
 */
#ifndef FORMBROD_PLAYWINDOW_H
#define FORMBROD_PLAYWINDOW_H

#include <exec/types.h>
#include "formbrod/player.h"

/* Opens the window (w x h is the picture to come; 0 = a default size), or
 * brings it to front, and says the preview is being prepared until the
 * first picture is shown. */
void   PlayWindow_Prepare(int w, int h);
/* Opens the window (as Prepare) or brings it to front, keeping the
 * picture it shows. */
void   PlayWindow_Open(int w, int h);
/* Text in the picture area while there is no picture yet, e.g. why the
 * preview stopped before its first one. */
void   PlayWindow_SetMessage(const char *text);
/* Whether the window is open. While it is and nothing plays, it shows
 * the frame at the playhead (main.c, App_StillShow). */
int    PlayWindow_IsOpen(void);
/* Whether a picture of this preview has been shown since Prepare. */
int    PlayWindow_HasPicture(void);
/* Shows a picture, opening the window (or making it fit) if needed. */
void   PlayWindow_Show(const FBPlayFrame *f);
/* The line under the picture. */
void   PlayWindow_SetText(const char *text);
uint32 PlayWindow_SigMask(void);
void   PlayWindow_Handle(uint32 sigs);
void   PlayWindow_Close(void);

#endif
