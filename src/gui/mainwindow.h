#ifndef FORMBROD_MAINWINDOW_H
#define FORMBROD_MAINWINDOW_H

#include <exec/types.h>
#include <intuition/pointerclass.h>
#include "formbrod/backend.h"

/* The pointer for "working, but you can go on": an arrow with an
 * hourglass (OS4's progress pointer), else the busy one. */
#ifdef POINTERTYPE_PROGRESS
#define FB_WORKING_POINTER POINTERTYPE_PROGRESS
#else
#define FB_WORKING_POINTER POINTERTYPE_BUSY
#endif

/* Shows the arrow-with-hourglass pointer over both windows while proxies
 * are made or an export runs (and the normal one again after). */
void MainWindow_SetWorking(BOOL working);

int    MainWindow_Open(void);

/* Its own and the clip window's signals. */
uint32 MainWindow_SigMask(void);
void   MainWindow_Handle(uint32 sigs);
void   MainWindow_Refresh(void);
void   MainWindow_SetProgress(const FBProgress *p);
void   MainWindow_Close(void);

#endif
