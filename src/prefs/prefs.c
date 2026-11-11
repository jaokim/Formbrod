/*
 * prefs.c - program settings through application.library PrefsObjects.
 *
 * The library loads the file when Formbrod registers (REGAPP_LoadPrefs) and
 * hands us its main dictionary. Keys are flat strings in that dictionary:
 *
 *   MediaDrawer, ProjectDrawer, ExportDrawer   file requester drawers
 *
 * The interfaces are kept in our own variables rather than IApplication and
 * IPrefsObjects, so -lauto doesn't open an older version of them for us:
 * PrefsObjects as used here needs version 2 of both.
 */
#include <string.h>

#include <proto/exec.h>
#include <proto/application.h>

#include "prefs/prefs.h"

#define APP_NAME   "Formbrod"
#define APP_URL    "jaokim.github.io"   /* with APP_NAME, names the file */

static struct Library *AppBase;
static struct ApplicationIFace *IApp;
static struct PrefsObjectsIFace *IPO;
static uint32 AppID;
static PrefsObject *Dict;   /* owned by the library */

int Prefs_Open(void)
{
    AppBase = IExec->OpenLibrary("application.library", 53);
    if (!AppBase)
        return -1;
    IApp = (struct ApplicationIFace *)
        IExec->GetInterface(AppBase, "application", 2, NULL);
    IPO = (struct PrefsObjectsIFace *)
        IExec->GetInterface(AppBase, "prefsobjects", 2, NULL);
    if (!IApp || !IPO)
        goto fail;

    AppID = IApp->RegisterApplication(APP_NAME,
        REGAPP_URLIdentifier, APP_URL,
        REGAPP_LoadPrefs,     TRUE,
        REGAPP_NoIcon,        TRUE,
        TAG_END);
    if (!AppID)
        goto fail;
    IApp->GetApplicationAttrs(AppID, APPATTR_MainPrefsDict, &Dict, TAG_END);
    if (!Dict)
        goto fail;
    return 0;

fail:
    Prefs_Close();
    return -1;
}

void Prefs_Close(void)
{
    Dict = NULL;
    if (AppID) {
        IApp->UnregisterApplication(AppID, NULL);
        AppID = 0;
    }
    if (IPO) {
        IExec->DropInterface((struct Interface *)IPO);
        IPO = NULL;
    }
    if (IApp) {
        IExec->DropInterface((struct Interface *)IApp);
        IApp = NULL;
    }
    if (AppBase) {
        IExec->CloseLibrary(AppBase);
        AppBase = NULL;
    }
}

const char *Prefs_GetString(const char *key, const char *def)
{
    if (!Dict)
        return def;
    return (const char *)IPO->DictGetStringForKey(Dict, key, def);
}

void Prefs_SetString(const char *key, const char *value)
{
    PrefsObject *str;

    if (!Dict)
        return;
    str = IPO->PrefsString(NULL, NULL, ALPOSTR_AllocSetString, value, TAG_END);
    if (str)
        IPO->DictSetObjectForKey(Dict, str, key);
}

int Prefs_Save(void)
{
    if (!Dict)
        return -1;
    return IPO->WritePrefs(Dict,
        WRITEPREFS_AppID,       AppID,
        WRITEPREFS_WriteENV,    TRUE,
        WRITEPREFS_WriteENVARC, TRUE,
        TAG_END) == 0 ? 0 : -1;
}
