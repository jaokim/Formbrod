/*
 * playwindow.h - the preview player's window: the picture, and the time
 * under it.
 *
 * It opens when the first picture of a preview arrives (its size is the
 * picture's) and stays open after the preview stops, showing the last
 * picture. Closing it, or Esc, stops the preview (App_PlayStop).
 */
#ifndef FORMBROD_PLAYWINDOW_H
#define FORMBROD_PLAYWINDOW_H

#include <exec/types.h>
#include "formbrod/player.h"

/* Shows a picture, opening the window (or making it fit) if needed. */
void   PlayWindow_Show(const FBPlayFrame *f);
/* The line under the picture. */
void   PlayWindow_SetText(const char *text);
uint32 PlayWindow_SigMask(void);
void   PlayWindow_Handle(uint32 sigs);
void   PlayWindow_Close(void);

#endif
