/*
 * Host-side test for src/locale/catalog.c: without locale.library every
 * string falls back to the built-in English text.
 * Build and run with:  make -f Makefile.host test
 */
#include <stdio.h>
#include <string.h>
#define FB_STRINGS_ARRAY        /* also gives this file the table */
#include "locale/catalog.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

int main(void)
{
    int i, j;

    /* Usable before Locale_Open(). */
    CHECK(strcmp(FB_Str(MSG_BTN_SPLIT), "_Split") == 0);

    Locale_Open();
    CHECK(strcmp(FB_Str(MSG_WINDOW_TITLE), "Formbr\366d") == 0);
    CHECK(strcmp(FB_Str(MSG_STATUS_ADDED), "Added %s") == 0);
    CHECK(strcmp(FB_Str(99999), "") == 0);     /* unknown ID: never NULL */
    CHECK(Locale_DecimalPoint() == '.');

    for (i = 0; i < FB_NUM_STRINGS; i++) {
        CHECK(FB_Str(FB_BuiltinStrings[i].id) == FB_BuiltinStrings[i].str);
        for (j = i + 1; j < FB_NUM_STRINGS; j++)
            CHECK(FB_BuiltinStrings[i].id != FB_BuiltinStrings[j].id);
    }
    Locale_Close();

    if (failures) {
        printf("%d locale test(s) failed\n", failures);
        return 1;
    }
    printf("Locale tests passed\n");
    return 0;
}
