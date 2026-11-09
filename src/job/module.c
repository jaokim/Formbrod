#include <string.h>
#include "formbrod/module.h"

#define FB_MAX_MODULES 64

static const FBModule *modules[FB_MAX_MODULES];
static int num_modules;

int FB_RegisterModule(const FBModule *mod)
{
    if (!mod || mod->api != FB_MODULE_API || !mod->name)
        return -1;
    if (FB_FindModule(mod->name))
        return -1;           /* duplicate name */
    if (num_modules >= FB_MAX_MODULES)
        return -1;
    modules[num_modules++] = mod;
    return 0;
}

const FBModule *FB_FindModule(const char *name)
{
    int i;

    for (i = 0; i < num_modules; i++)
        if (strcmp(modules[i]->name, name) == 0)
            return modules[i];
    return NULL;
}

int FB_NumModules(void)
{
    return num_modules;
}

const FBModule *FB_GetModule(int index)
{
    if (index < 0 || index >= num_modules)
        return NULL;
    return modules[index];
}

/* Add new built-in modules here. */
extern const FBModule FB_Module_Fade;

void FB_RegisterBuiltinModules(void)
{
    FB_RegisterModule(&FB_Module_Fade);
}
