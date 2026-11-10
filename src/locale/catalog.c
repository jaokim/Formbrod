/*
 * catalog.c - translated GUI strings through locale.library.
 *
 * locale.library looks for formbrod.catalog in PROGDIR:Catalogs/<language>/
 * and LOCALE:Catalogs/<language>/, in the order of the user's preferred
 * languages. Catalogs are built from the locale/ .ct files with 'make catalogs'.
 *
 * Off AmigaOS (host checks and tests) only the built-in strings are used.
 */
#define FB_STRINGS_ARRAY
#include "locale/catalog.h"

#ifdef __amigaos4__
#include <proto/exec.h>
#include <proto/locale.h>

static struct Catalog *Cat;
static struct Locale  *Loc;
#endif

static const char *builtin(long id)
{
    int i;

    for (i = 0; i < FB_NUM_STRINGS; i++)
        if (FB_BuiltinStrings[i].id == id)
            return FB_BuiltinStrings[i].str;
    return "";
}

void Locale_Open(void)
{
#ifdef __amigaos4__
    /* ILocale is opened by -lauto; it is NULL if locale.library is absent. */
    if (!ILocale)
        return;
    Loc = ILocale->OpenLocale(NULL);
    Cat = ILocale->OpenCatalog(NULL, FB_CATALOG_NAME,
        OC_BuiltInLanguage, "english",
        OC_Version,         FB_CATALOG_VERSION,
        TAG_END);
#endif
}

void Locale_Close(void)
{
#ifdef __amigaos4__
    if (ILocale) {
        ILocale->CloseCatalog(Cat);     /* both accept NULL */
        ILocale->CloseLocale(Loc);
    }
    Cat = NULL;
    Loc = NULL;
#endif
}

const char *FB_Str(long id)
{
    const char *def = builtin(id);

#ifdef __amigaos4__
    if (Cat && ILocale)
        return (const char *)ILocale->GetCatalogStr(Cat, id, (STRPTR)def);
#endif
    return def;
}

char Locale_DecimalPoint(void)
{
#ifdef __amigaos4__
    if (Loc && Loc->loc_DecimalPoint && Loc->loc_DecimalPoint[0])
        return (char)Loc->loc_DecimalPoint[0];
#endif
    return '.';
}
