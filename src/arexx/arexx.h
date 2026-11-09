#ifndef FORMBROD_AREXX_H
#define FORMBROD_AREXX_H

#include <exec/types.h>

int    ARexx_Init(void);       /* 0 on success; the app runs without it too */
uint32 ARexx_SigMask(void);
void   ARexx_Handle(void);
void   ARexx_Cleanup(void);

#endif
