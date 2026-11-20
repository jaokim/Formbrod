/*
 * projectwindow.h - the project settings window: the frame (size, rate,
 * fit, bar colour) and how the project is exported (file type, picture
 * and sound encoders, quality, bit rate, sample rate). All saved in the
 * project file.
 */
#ifndef FORMBROD_PROJECTWINDOW_H
#define FORMBROD_PROJECTWINDOW_H

#include <exec/types.h>

/* Opens the window, or brings it to the front. */
void   ProjectWindow_Open(void);
/* Shows the project's settings again (after any change). */
void   ProjectWindow_Refresh(void);
uint32 ProjectWindow_SigMask(void);   /* 0 while it's closed */
void   ProjectWindow_SetWorking(BOOL working);
void   ProjectWindow_Handle(uint32 sigs);
/* Closes it and lets go of its classes; at the end of the program. */
void   ProjectWindow_Close(void);

#endif
