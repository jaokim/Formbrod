#ifndef FORMBROD_MAINWINDOW_H
#define FORMBROD_MAINWINDOW_H

#include <exec/types.h>
#include "formbrod/backend.h"

int    MainWindow_Open(void);
uint32 MainWindow_SigMask(void);
void   MainWindow_Handle(uint32 sigs);
void   MainWindow_Refresh(void);
void   MainWindow_SetProgress(const FBProgress *p);
void   MainWindow_Close(void);

#endif
