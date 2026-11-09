/*
 * arexx.c - the FORMBROD ARexx port, built on arexx.class.
 *
 * Commands map one-to-one onto App_* actions (see app.h). To add one:
 * write a handler, add a row to `commands`, and document it in README.md.
 *
 *   OPEN FILE/A                         add a file as a clip
 *   SPLIT AT/N/A                        split at AT milliseconds
 *   REMOVE CLIP/N/A
 *   EFFECT CLIP/N/A,MODULE/A            add an effect, returns its index
 *   SETPARAM CLIP/N/A,EFFECT/N/A,NAME/A,VALUE/A
 *   EXPORT FILE/A,PREVIEW/S
 *   CANCEL
 *   RENDERSTATUS                        IDLE|RUNNING <pos> <total>|DONE|FAILED
 *   CLIPS                               number of clips
 *   MODULES                             space-separated module names
 *   QUIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <classes/arexx.h>

#include "app.h"
#include "arexx.h"
#include "formbrod/module.h"

static struct ClassLibrary *ARexxBase;
static Class *ARexxClass;
static Object *ARexxObj;

static char result[512];

static void set_result(struct ARexxCmd *cmd, int rc)
{
    cmd->ac_RC = rc;
    cmd->ac_Result = result[0] ? result : NULL;
}

static void cmd_open(struct ARexxCmd *cmd, struct RexxMsg *rm)
{
    (void)rm;
    result[0] = '\0';
    set_result(cmd, App_AddFile((STRPTR)cmd->ac_ArgList[0]) ? RC_ERROR : 0);
}

static void cmd_split(struct ARexxCmd *cmd, struct RexxMsg *rm)
{
    (void)rm;
    result[0] = '\0';
    set_result(cmd, App_Split(*(int32 *)cmd->ac_ArgList[0]) < 0 ? RC_ERROR : 0);
}

static void cmd_remove(struct ARexxCmd *cmd, struct RexxMsg *rm)
{
    (void)rm;
    result[0] = '\0';
    set_result(cmd, App_RemoveClip(*(int32 *)cmd->ac_ArgList[0]) ? RC_ERROR : 0);
}

static void cmd_effect(struct ARexxCmd *cmd, struct RexxMsg *rm)
{
    int idx;

    (void)rm;
    idx = App_AddEffect(*(int32 *)cmd->ac_ArgList[0],
                        (const char *)cmd->ac_ArgList[1]);
    result[0] = '\0';
    if (idx >= 0)
        snprintf(result, sizeof(result), "%d", idx);
    set_result(cmd, idx < 0 ? RC_ERROR : 0);
}

static void cmd_setparam(struct ARexxCmd *cmd, struct RexxMsg *rm)
{
    (void)rm;
    result[0] = '\0';
    set_result(cmd, App_SetEffectParam(*(int32 *)cmd->ac_ArgList[0],
                                       *(int32 *)cmd->ac_ArgList[1],
                                       (const char *)cmd->ac_ArgList[2],
                                       atof((const char *)cmd->ac_ArgList[3]))
                    ? RC_ERROR : 0);
}

static void cmd_export(struct ARexxCmd *cmd, struct RexxMsg *rm)
{
    (void)rm;
    result[0] = '\0';
    set_result(cmd, App_Export((const char *)cmd->ac_ArgList[0],
                               cmd->ac_ArgList[1] != 0) ? RC_ERROR : 0);
}

static void cmd_cancel(struct ARexxCmd *cmd, struct RexxMsg *rm)
{
    (void)rm;
    App_CancelRender();
    result[0] = '\0';
    set_result(cmd, 0);
}

static void cmd_renderstatus(struct ARexxCmd *cmd, struct RexxMsg *rm)
{
    FBProgress p = App_PollRender();

    (void)rm;
    switch (p.state) {
    case FB_RENDER_RUNNING:
        snprintf(result, sizeof(result), "RUNNING %ld %ld",
                 (long)p.position, (long)p.total);
        break;
    case FB_RENDER_DONE:      strcpy(result, "DONE");      break;
    case FB_RENDER_FAILED:    strcpy(result, "FAILED");    break;
    case FB_RENDER_CANCELLED: strcpy(result, "CANCELLED"); break;
    default:                  strcpy(result, "IDLE");      break;
    }
    set_result(cmd, 0);
}

static void cmd_clips(struct ARexxCmd *cmd, struct RexxMsg *rm)
{
    (void)rm;
    snprintf(result, sizeof(result), "%d", App.timeline.num_clips);
    set_result(cmd, 0);
}

static void cmd_modules(struct ARexxCmd *cmd, struct RexxMsg *rm)
{
    size_t used = 0;
    int i;

    (void)rm;
    result[0] = '\0';
    for (i = 0; i < FB_NumModules(); i++) {
        int n = snprintf(result + used, sizeof(result) - used, "%s%s",
                         i ? " " : "", FB_GetModule(i)->name);
        if (n < 0 || (size_t)n >= sizeof(result) - used)
            break;
        used += n;
    }
    set_result(cmd, 0);
}

static void cmd_quit(struct ARexxCmd *cmd, struct RexxMsg *rm)
{
    (void)rm;
    App.quit = 1;
    result[0] = '\0';
    set_result(cmd, 0);
}

static struct ARexxCmd commands[] = {
    { "OPEN",         1,  cmd_open,         "FILE/A",            0, NULL, 0, 0, NULL },
    { "SPLIT",        2,  cmd_split,        "AT/N/A",            0, NULL, 0, 0, NULL },
    { "REMOVE",       3,  cmd_remove,       "CLIP/N/A",          0, NULL, 0, 0, NULL },
    { "EFFECT",       4,  cmd_effect,       "CLIP/N/A,MODULE/A", 0, NULL, 0, 0, NULL },
    { "SETPARAM",     5,  cmd_setparam,
      "CLIP/N/A,EFFECT/N/A,NAME/A,VALUE/A",                       0, NULL, 0, 0, NULL },
    { "EXPORT",       6,  cmd_export,       "FILE/A,PREVIEW/S",  0, NULL, 0, 0, NULL },
    { "CANCEL",       7,  cmd_cancel,       NULL,                0, NULL, 0, 0, NULL },
    { "RENDERSTATUS", 8,  cmd_renderstatus, NULL,                0, NULL, 0, 0, NULL },
    { "CLIPS",        9,  cmd_clips,        NULL,                0, NULL, 0, 0, NULL },
    { "MODULES",      10, cmd_modules,      NULL,                0, NULL, 0, 0, NULL },
    { "QUIT",         11, cmd_quit,         NULL,                0, NULL, 0, 0, NULL },
    { NULL,           0,  NULL,             NULL,                0, NULL, 0, 0, NULL }
};

int ARexx_Init(void)
{
    ARexxBase = IIntuition->OpenClass("arexx.class", 52, &ARexxClass);
    if (!ARexxBase)
        return -1;
    ARexxObj = IIntuition->NewObject(ARexxClass, NULL,
        AREXX_HostName, "FORMBROD",
        AREXX_Commands, commands,
        AREXX_NoSlot,   TRUE,
        TAG_END);
    return ARexxObj ? 0 : -1;
}

uint32 ARexx_SigMask(void)
{
    uint32 mask = 0;

    if (ARexxObj)
        IIntuition->GetAttr(AREXX_SigMask, ARexxObj, &mask);
    return mask;
}

void ARexx_Handle(void)
{
    if (ARexxObj)
        IIntuition->IDoMethod(ARexxObj, AM_HANDLEEVENT);
}

void ARexx_Cleanup(void)
{
    if (ARexxObj) {
        IIntuition->DisposeObject(ARexxObj);
        ARexxObj = NULL;
    }
    if (ARexxBase) {
        IIntuition->CloseClass(ARexxBase);
        ARexxBase = NULL;
    }
}
