#ifndef FORMBROD_PREFS_H
#define FORMBROD_PREFS_H

/*
 * prefs.h - program settings, kept with application.library's PrefsObjects
 * in ENV(ARC):Formbrod.jaokim.github.io.xml.
 *
 * These are settings for the program, not for one project: the drawers
 * the file requesters last used, and later things like window size or
 * render servers. Projects have their own text format (formbrod/project.h).
 *
 * Without application.library (or with an old one) Prefs_Open() fails and
 * the getters return their defaults; nothing is remembered between runs.
 */

/* Registers Formbrod with application.library and loads the settings.
 * 0 on success. Safe to call Prefs_Close() either way. */
int  Prefs_Open(void);
void Prefs_Close(void);

/* The string stored under `key`, or `def`. The result is valid until the
 * key is set again. */
const char *Prefs_GetString(const char *key, const char *def);

/* Sets a string. Nothing is written until Prefs_Save(). */
void Prefs_SetString(const char *key, const char *value);

/* Writes the settings to ENV: and ENVARC:. 0 on success. */
int  Prefs_Save(void);

#endif
