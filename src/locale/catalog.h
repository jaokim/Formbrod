/*
 * catalog.h - translated GUI strings through locale.library.
 *
 *   App_SetStatus(FB_Str(MSG_STATUS_ADDED), path);
 *
 * String IDs and the built-in English text come from locale/formbrod.cd via
 * the generated strings.h. If no catalog matches the user's language (or
 * locale.library is missing), FB_Str() returns the English text, so it
 * never returns NULL and can be called before Locale_Open().
 *
 * ARexx commands and results are not translated: scripts must work the
 * same everywhere.
 */
#ifndef FORMBROD_CATALOG_H
#define FORMBROD_CATALOG_H

#include "locale/strings.h"

/* Opens the catalog for the user's preferred languages. Safe to call when
 * locale.library is missing. Call before App_Init(). */
void        Locale_Open(void);
void        Locale_Close(void);

/* The string with this MSG_ ID, translated if possible. */
const char *FB_Str(long id);

/* The decimal point of the user's locale, e.g. '.' or ','. */
char        Locale_DecimalPoint(void);

#endif
