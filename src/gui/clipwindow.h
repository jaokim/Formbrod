/*
 * clipwindow.h - the clip window: every setting of the selected clip
 * (timing, crossfade, fades, fit, volume, effects) in one window. It
 * follows the selection in the main window and stays open beside it.
 */
#ifndef FORMBROD_CLIPWINDOW_H
#define FORMBROD_CLIPWINDOW_H

#include <exec/types.h>

/* Opens the window, or brings it to the front. */
void   ClipWindow_Open(void);
/* Shows the main window's selected clip again (after any change). */
void   ClipWindow_Refresh(void);
uint32 ClipWindow_SigMask(void);      /* 0 while it's closed */
void   ClipWindow_Handle(uint32 sigs);
/* Closes it and lets go of its classes; at the end of the program. */
void   ClipWindow_Close(void);

/* mainwindow.c: the clip selected there, or -1. */
int    MainWindow_SelectedClip(void);

#endif
