/*
 * clipwindow.c - the clip window: every setting of one clip.
 *
 * Opened by double-clicking a clip in the track editor, or with Edit >
 * Clip settings. There is one, and it shows whichever clip is selected in
 * the main window, so it can stay open beside the timeline. Every change
 * goes through an App_* action (as the main window's and ARexx do) and is
 * applied when a field is left with Return or Tab, or a chooser is picked.
 *
 * Groups:
 *   Timing   Starts, From file, Length, Crossfade and its kind
 *   Fades    Fade in and Fade out (the "fade" effect)
 *   Picture  Fit, what the file holds, and what fitting does to it
 *   Sound    Volume
 *   Effects  the clip's other effects, one at a time, with a field per
 *            parameter made from what the module declares. A new module
 *            gets its settings here without any GUI code.
 *
 * Not yet compiled against the SDK: treat ReAction details as unverified.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>

#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/button.h>
#include <gadgets/string.h>
#include <gadgets/chooser.h>
#include <images/label.h>
#include <images/bevel.h>

#include "app.h"
#include "formbrod/module.h"
#include "formbrod/timeview.h"
#include "locale/catalog.h"
#include "gui/clipwindow.h"
#include "gui/mainwindow.h"
#include "gui/xfadekinds.h"

/* How many parameters of an effect are shown. Modules with more show the
 * first ones (FB_MAX_PARAMS is the most an effect can have). */
#define PARAM_ROWS 4

enum {
    CG_NAME = 1,
    CG_STARTS, CG_IN, CG_LENGTH, CG_XFADE, CG_KIND,
    CG_FADE_IN, CG_FADE_OUT,
    CG_FIT, CG_SOURCE, CG_SHOWN,
    CG_VOLUME,
    CG_EFFECT, CG_PREV, CG_NEXT, CG_REMOVE,
    CG_PARAM_LABEL,                      /* + row */
    CG_PARAM = CG_PARAM_LABEL + PARAM_ROWS,  /* + row */
    CG_LAST = CG_PARAM + PARAM_ROWS
};

enum { K_WINDOW, K_LAYOUT, K_BUTTON, K_STRING, K_CHOOSER, K_LABEL,
       K_COUNT };

static struct {
    struct ClassLibrary *base;
    Class *cls;
    const char *name;
} classes[] = {
    { NULL, NULL, "window.class" },
    { NULL, NULL, "gadgets/layout.gadget" },
    { NULL, NULL, "gadgets/button.gadget" },
    { NULL, NULL, "gadgets/string.gadget" },
    { NULL, NULL, "gadgets/chooser.gadget" },
    { NULL, NULL, "images/label.image" },
};
#define CLS(k) (classes[k].cls)

static Object *WinObj;
static struct Window *Win;
static struct Gadget *Gad[CG_LAST];

/* Texts the gadgets point at. */
static char NameText[160], SourceText[80], ShownText[64], EffectText[96];
static char FieldText[CG_LAST][32];
static char ParamLabel[PARAM_ROWS][64];

/* Chooser labels, from the catalog when the window opens. */
static STRPTR KindLabels[NUM_XFADE_KINDS + 1];
static STRPTR FitLabels[FB_FIT_COUNT + 1];

/* Which of the clip's other effects is shown (0 = the first). */
static int EffectPos;

/* Whether the working pointer is wanted (also for when it opens). */
static BOOL Working;

/* ---- Making the gadgets ---------------------------------------------- */

static Object *label(const char *text)
{
    return IIntuition->NewObject(CLS(K_LABEL), NULL, LABEL_Text, text,
                                 TAG_END);
}

static Object *field(uint32 id)
{
    return (Object *)(Gad[id] = (struct Gadget *)IIntuition->NewObject(
            CLS(K_STRING), NULL,
        GA_ID,            id,
        GA_RelVerify,     TRUE,
        GA_TabCycle,      TRUE,
        GA_Disabled,      TRUE,
        STRINGA_MaxChars, sizeof(FieldText[0]) - 1,
        STRINGA_TextVal,  "",
        TAG_END));
}

static Object *text(uint32 id, char *buf)
{
    return (Object *)(Gad[id] = (struct Gadget *)IIntuition->NewObject(
            CLS(K_BUTTON), NULL,
        GA_ID,       id,
        GA_ReadOnly, TRUE,
        GA_Text,     buf,
        BUTTON_Justification, BCJ_LEFT,
        TAG_END));
}

static Object *button(uint32 id, const char *label_text)
{
    return (Object *)(Gad[id] = (struct Gadget *)IIntuition->NewObject(
            CLS(K_BUTTON), NULL,
        GA_ID,        id,
        GA_RelVerify, TRUE,
        GA_Disabled,  TRUE,
        GA_Text,      label_text,
        TAG_END));
}

static Object *chooser(uint32 id, STRPTR *labels)
{
    return (Object *)(Gad[id] = (struct Gadget *)IIntuition->NewObject(
            CLS(K_CHOOSER), NULL,
        GA_ID,              id,
        GA_RelVerify,       TRUE,
        GA_Disabled,        TRUE,
        CHOOSER_LabelArray, labels,
        CHOOSER_Selected,   0,
        TAG_END));
}

/* ---- Showing the clip ------------------------------------------------- */

/* Sets a gadget's text or number and whether it can be used; redrawn here,
 * since a window shows a change of GA_Disabled only when refreshed. */
static void set_gad(uint32 id, uint32 tag, uint32 value, BOOL disabled)
{
    if (IIntuition->SetGadgetAttrs(Gad[id], Win, NULL,
                                   tag,         value,
                                   GA_Disabled, disabled,
                                   TAG_END) && Win)
        IIntuition->RefreshGList(Gad[id], Win, NULL, 1);
}

static void set_field(uint32 id, BOOL disabled)
{
    if (disabled)
        FieldText[id][0] = '\0';
    set_gad(id, STRINGA_TextVal, (uint32)FieldText[id], disabled);
}

static void set_text(uint32 id)
{
    set_gad(id, GA_Text, (uint32)(id == CG_NAME   ? NameText :
                                  id == CG_SOURCE ? SourceText :
                                  id == CG_SHOWN  ? ShownText : EffectText),
            FALSE);
}

static void format_ms(uint32 id, fb_ms_t ms)
{
    FB_FormatTimeMs(FieldText[id], sizeof(FieldText[id]), ms,
                    Locale_DecimalPoint());
}

/* A number as typed here: with the locale's decimal point. */
static void format_number(char *buf, size_t len, double v)
{
    char dp = Locale_DecimalPoint(), *p;

    snprintf(buf, len, "%g", v);
    if (dp != '.' && (p = strchr(buf, '.')))
        *p = dp;
}

static int parse_number(const char *s, double *v)
{
    char buf[32], dp = Locale_DecimalPoint(), *p, *end;

    snprintf(buf, sizeof(buf), "%s", s);
    if (dp != '.' && (p = strchr(buf, dp)))
        *p = '.';
    *v = strtod(buf, &end);
    while (*end == ' ')
        end++;
    return end != buf && *end == '\0' ? 0 : -1;
}

/* What fitting does to the clip, as the main window's inspector says it. */
static const char *fit_text(FBFitResult r)
{
    switch (r) {
    case FB_FITS_SCALE:     return FB_Str(MSG_FITS_SCALE);
    case FB_FITS_LETTERBOX: return FB_Str(MSG_FITS_LETTERBOX);
    case FB_FITS_PILLARBOX: return FB_Str(MSG_FITS_PILLARBOX);
    case FB_FITS_CROP:      return FB_Str(MSG_FITS_CROP);
    case FB_FITS_STRETCH:   return FB_Str(MSG_FITS_STRETCH);
    default:                return FB_Str(MSG_FITS_AS_IS);
    }
}

/* The clip's effects other than its fades, which have a group of their
 * own: their indices. */
static int other_effects(const FBClip *c, int *idx)
{
    int i, n = 0;

    for (i = 0; i < c->num_effects; i++)
        if (strcmp(c->effects[i].module, "fade") != 0)
            idx[n++] = i;
    return n;
}

static void show_effects(const FBClip *c)
{
    int idx[FB_MAX_EFFECTS], n = c ? other_effects(c, idx) : 0, r;
    const FBEffect *fx = NULL;
    const FBModule *mod = NULL;

    if (EffectPos >= n)
        EffectPos = n - 1;
    if (EffectPos < 0)
        EffectPos = 0;
    if (n > 0) {
        fx = &c->effects[idx[EffectPos]];
        mod = FB_FindModule(fx->module);
        snprintf(EffectText, sizeof(EffectText), FB_Str(MSG_EFFECT_OF),
                 mod && mod->label ? mod->label : fx->module,
                 (long)(EffectPos + 1), (long)n);
    } else {
        snprintf(EffectText, sizeof(EffectText), "%s",
                 FB_Str(c ? MSG_EFFECTS_NONE : MSG_CLIPWIN_NONE));
    }
    set_text(CG_EFFECT);
    set_gad(CG_PREV, GA_Text, (uint32)FB_Str(MSG_BTN_PREV_EFFECT),
            EffectPos <= 0);
    set_gad(CG_NEXT, GA_Text, (uint32)FB_Str(MSG_BTN_NEXT_EFFECT),
            EffectPos >= n - 1);
    set_gad(CG_REMOVE, GA_Text, (uint32)FB_Str(MSG_BTN_REMOVE_EFFECT),
            n == 0);

    /* A row per declared parameter (an unknown module: its own). */
    for (r = 0; r < PARAM_ROWS; r++) {
        BOOL used = FALSE;

        ParamLabel[r][0] = '\0';
        if (mod && r < mod->num_params) {
            const FBParamDef *def = &mod->params[r];

            snprintf(ParamLabel[r], sizeof(ParamLabel[r]), "%s",
                     def->label ? def->label : def->name);
            format_number(FieldText[CG_PARAM + r], sizeof(FieldText[0]),
                          FB_EffectGetParam(fx, def->name, def->def));
            used = TRUE;
        } else if (fx && !mod && r < fx->num_params) {
            snprintf(ParamLabel[r], sizeof(ParamLabel[r]), "%s",
                     fx->params[r].name);
            format_number(FieldText[CG_PARAM + r], sizeof(FieldText[0]),
                          fx->params[r].value);
            used = TRUE;
        }
        set_gad(CG_PARAM_LABEL + r, GA_Text, (uint32)ParamLabel[r], FALSE);
        set_field(CG_PARAM + r, !used);
    }
}

void ClipWindow_Refresh(void)
{
    const FBTimeline *tl = &App.timeline;
    int clip = MainWindow_SelectedClip(), i;
    const FBClip *c = clip >= 0 ? &tl->clips[clip] : NULL;
    const FBSource *src = c ? &tl->sources[c->source] : NULL;
    BOOL none = !c, still = src && src->still;
    BOOL picture = src && src->has_video, sound = src && src->has_audio;
    BOOL no_xfade = none || (c->xfade <= 0 &&
                             FB_TimelineMaxCrossfade(tl, clip) <= 0);
    fb_ms_t fin = 0, fout = 0;

    if (!WinObj || !Win)
        return;

    /* Name and timing. */
    snprintf(NameText, sizeof(NameText), "%s",
             none ? FB_Str(MSG_CLIPWIN_NONE) : IDOS->FilePart(src->path));
    set_text(CG_NAME);
    if (c) {
        format_ms(CG_STARTS, c->start);
        format_ms(CG_IN, c->in);
        format_ms(CG_LENGTH, c->out - c->in);
        format_ms(CG_XFADE, c->xfade);
    }
    set_field(CG_STARTS, none);
    set_field(CG_IN, none || still);
    set_field(CG_LENGTH, none);
    set_field(CG_XFADE, no_xfade);
    i = c ? xfade_kind_index(FB_ClipCrossfadeKind(c)) : 0;
    /* A kind from ARexx that the chooser doesn't have: shown as it is in
     * the main window, and left alone here. */
    set_gad(CG_KIND, CHOOSER_Selected, (uint32)(i >= 0 ? i : 0),
            no_xfade || i < 0);

    /* Fades. */
    App_GetFades(clip, &fin, &fout);
    format_ms(CG_FADE_IN, fin);
    format_ms(CG_FADE_OUT, fout);
    set_field(CG_FADE_IN, none);
    set_field(CG_FADE_OUT, none);

    /* Picture: its fit, the file's picture, and what fitting does. */
    set_gad(CG_FIT, CHOOSER_Selected, c ? (uint32)c->fit : 0,
            none || !picture);
    if (!picture) {
        snprintf(SourceText, sizeof(SourceText), "%s",
                 none ? "" : FB_Str(MSG_SOURCE_NO_PICTURE));
        ShownText[0] = '\0';
    } else {
        int fw = 0, fh = 0;

        if (still) {
            snprintf(SourceText, sizeof(SourceText),
                     FB_Str(MSG_SOURCE_STILL), (long)src->width,
                     (long)src->height);
        } else {
            char fps[16];

            if (src->fps_num > 0 && src->fps_den > 0)
                format_number(fps, sizeof(fps),
                              (double)((long)((double)src->fps_num /
                                       src->fps_den * 100.0 + 0.5)) / 100.0);
            else
                snprintf(fps, sizeof(fps), "?");
            snprintf(SourceText, sizeof(SourceText),
                     FB_Str(MSG_SOURCE_PICTURE), (long)src->width,
                     (long)src->height, fps);
        }
        FB_TimelineFrameSize(tl, &fw, &fh);
        snprintf(ShownText, sizeof(ShownText), "%s",
                 fit_text(FB_ClipFitResult(tl, c, fw, fh)));
    }
    set_text(CG_SOURCE);
    set_text(CG_SHOWN);

    /* Sound. */
    if (c)
        snprintf(FieldText[CG_VOLUME], sizeof(FieldText[0]), "%ld",
                 (long)(c->volume * 100.0 + 0.5));
    set_field(CG_VOLUME, none || !sound);

    show_effects(c);
}

/* ---- Changes --------------------------------------------------------- */

static const char *field_text(uint32 id)
{
    STRPTR t = NULL;

    IIntuition->GetAttr(STRINGA_TextVal, (Object *)Gad[id], (uint32 *)&t);
    return t ? (const char *)t : "";
}

/* A typed time, or -1 with a word in the status line. */
static fb_ms_t field_time(uint32 id)
{
    const char *t = field_text(id);
    fb_ms_t ms;

    if (FB_ParseTime(t, Locale_DecimalPoint(), &ms) != 0) {
        App_SetStatus(FB_Str(MSG_STATUS_BAD_TIME), t);
        return -1;
    }
    return ms;
}

static void changed(uint32 id, int clip)
{
    const FBTimeline *tl = &App.timeline;
    const FBClip *c = &tl->clips[clip];
    uint32 sel = 0;
    fb_ms_t t;
    double v;

    switch (id) {
    case CG_STARTS:
        if ((t = field_time(id)) >= 0)
            App_MoveClip(clip, t);
        break;
    case CG_IN:
        if ((t = field_time(id)) >= 0) {
            fb_ms_t len = c->out - c->in;
            fb_ms_t dur = FB_SourceLimit(&tl->sources[c->source]);

            if (dur > 0 && t + len > dur)
                len = dur - t;
            App_TrimClip(clip, t, t + len);
        }
        break;
    case CG_LENGTH:
        if ((t = field_time(id)) >= 0)
            App_TrimClip(clip, c->in, c->in + t);
        break;
    case CG_XFADE:
        if ((t = field_time(id)) >= 0)
            App_SetCrossfade(clip, t, NULL);
        break;
    case CG_KIND:
        IIntuition->GetAttr(CHOOSER_Selected, (Object *)Gad[id], &sel);
        if (sel < (uint32)NUM_XFADE_KINDS)
            App_SetCrossfade(clip, c->xfade > 0 ? c->xfade
                                                : FB_CROSSFADE_LENGTH,
                             XfadeKinds[sel].name);
        break;
    case CG_FADE_IN:
        if ((t = field_time(id)) >= 0)
            App_SetFades(clip, t, -1);
        break;
    case CG_FADE_OUT:
        if ((t = field_time(id)) >= 0)
            App_SetFades(clip, -1, t);
        break;
    case CG_FIT:
        IIntuition->GetAttr(CHOOSER_Selected, (Object *)Gad[id], &sel);
        App_SetClipFit(clip, (int)sel);     /* 0 is FB_FIT_DEFAULT */
        break;
    case CG_VOLUME:
        if (parse_number(field_text(id), &v) == 0)
            App_SetVolume(clip, v / 100.0);
        else
            App_SetStatus(FB_Str(MSG_STATUS_BAD_NUMBER), field_text(id));
        break;
    case CG_PREV:
        EffectPos--;
        break;
    case CG_NEXT:
        EffectPos++;
        break;
    case CG_REMOVE:
    default: {
        int idx[FB_MAX_EFFECTS], n = other_effects(c, idx);
        const FBEffect *fx;
        const FBModule *mod;
        int r = (int)id - CG_PARAM;

        if (EffectPos < 0 || EffectPos >= n)
            break;
        if (id == CG_REMOVE) {
            App_RemoveEffect(clip, idx[EffectPos]);
            break;
        }
        if (r < 0 || r >= PARAM_ROWS)
            break;
        fx = &c->effects[idx[EffectPos]];
        mod = FB_FindModule(fx->module);
        if (parse_number(field_text(id), &v) != 0) {
            App_SetStatus(FB_Str(MSG_STATUS_BAD_NUMBER), field_text(id));
            break;
        }
        if (mod && r < mod->num_params) {
            const FBParamDef *def = &mod->params[r];

            if (v < def->min)
                v = def->min;
            if (v > def->max)
                v = def->max;
            App_SetEffectParam(clip, idx[EffectPos], def->name, v);
        } else if (!mod && r < fx->num_params) {
            App_SetEffectParam(clip, idx[EffectPos], fx->params[r].name, v);
        }
        break;
    }
    }
    MainWindow_Refresh();       /* which shows it here too */
}

/* ---- The window ------------------------------------------------------- */

static Object *make_layout(void)
{
    Object *timing, *fades, *picture, *sound, *effects, *params, *nav;
    Object *left, *right, *rows[PARAM_ROWS];
    int r;

    timing = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_BevelStyle,  BVS_GROUP,
        LAYOUT_Label,       FB_Str(MSG_GROUP_TIMING),
        LAYOUT_AddChild,    field(CG_STARTS),
        CHILD_Label,        label(FB_Str(MSG_COL_START)),
        LAYOUT_AddChild,    field(CG_IN),
        CHILD_Label,        label(FB_Str(MSG_LABEL_FROM_FILE)),
        LAYOUT_AddChild,    field(CG_LENGTH),
        CHILD_Label,        label(FB_Str(MSG_LABEL_LENGTH)),
        LAYOUT_AddChild,    field(CG_XFADE),
        CHILD_Label,        label(FB_Str(MSG_LABEL_CROSSFADE)),
        LAYOUT_AddChild,    chooser(CG_KIND, KindLabels),
        CHILD_Label,        label(FB_Str(MSG_LABEL_KIND)),
        TAG_END);

    fades = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_BevelStyle,  BVS_GROUP,
        LAYOUT_Label,       FB_Str(MSG_GROUP_FADES),
        LAYOUT_AddChild,    field(CG_FADE_IN),
        CHILD_Label,        label(FB_Str(MSG_LABEL_FADE_IN)),
        LAYOUT_AddChild,    field(CG_FADE_OUT),
        CHILD_Label,        label(FB_Str(MSG_LABEL_FADE_OUT)),
        TAG_END);

    picture = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_BevelStyle,  BVS_GROUP,
        LAYOUT_Label,       FB_Str(MSG_GROUP_PICTURE),
        LAYOUT_AddChild,    chooser(CG_FIT, FitLabels),
        CHILD_Label,        label(FB_Str(MSG_LABEL_FIT)),
        LAYOUT_AddChild,    text(CG_SOURCE, SourceText),
        CHILD_Label,        label(FB_Str(MSG_LABEL_SOURCE)),
        LAYOUT_AddChild,    text(CG_SHOWN, ShownText),
        CHILD_Label,        label(FB_Str(MSG_LABEL_SHOWN)),
        TAG_END);

    sound = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_BevelStyle,  BVS_GROUP,
        LAYOUT_Label,       FB_Str(MSG_GROUP_SOUND),
        LAYOUT_AddChild,    field(CG_VOLUME),
        CHILD_Label,        label(FB_Str(MSG_LABEL_VOLUME)),
        TAG_END);

    nav = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
        LAYOUT_EvenSize,    TRUE,
        LAYOUT_AddChild,    button(CG_PREV, FB_Str(MSG_BTN_PREV_EFFECT)),
        LAYOUT_AddChild,    button(CG_NEXT, FB_Str(MSG_BTN_NEXT_EFFECT)),
        LAYOUT_AddChild,    button(CG_REMOVE, FB_Str(MSG_BTN_REMOVE_EFFECT)),
        TAG_END);

    /* Labels that change with the effect: read-only buttons, since a
     * label image keeps the width it was laid out with. */
    for (r = 0; r < PARAM_ROWS; r++)
        rows[r] = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
            LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
            LAYOUT_AddChild,    text(CG_PARAM_LABEL + r, ParamLabel[r]),
            CHILD_WeightedWidth, 60,
            LAYOUT_AddChild,    field(CG_PARAM + r),
            CHILD_WeightedWidth, 40,
            TAG_END);
#if PARAM_ROWS != 4
#error "make_layout() adds PARAM_ROWS rows by hand"
#endif
    params = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_AddChild,    rows[0],
        LAYOUT_AddChild,    rows[1],
        LAYOUT_AddChild,    rows[2],
        LAYOUT_AddChild,    rows[3],
        TAG_END);

    effects = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_BevelStyle,  BVS_GROUP,
        LAYOUT_Label,       FB_Str(MSG_GROUP_EFFECTS),
        LAYOUT_AddChild,    text(CG_EFFECT, EffectText),
        LAYOUT_AddChild,    nav,
        CHILD_WeightedHeight, 0,
        LAYOUT_AddChild,    params,
        TAG_END);

    left = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_AddChild,    timing,
        CHILD_WeightedHeight, 0,
        LAYOUT_AddChild,    fades,
        CHILD_WeightedHeight, 0,
        LAYOUT_AddChild,    sound,
        CHILD_WeightedHeight, 0,
        TAG_END);
    right = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_AddChild,    picture,
        CHILD_WeightedHeight, 0,
        LAYOUT_AddChild,    effects,
        TAG_END);

    return IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_SpaceOuter,  TRUE,
        LAYOUT_AddChild,    text(CG_NAME, NameText),
        CHILD_WeightedHeight, 0,
        LAYOUT_AddChild,    IIntuition->NewObject(CLS(K_LAYOUT), NULL,
            LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
            LAYOUT_AddChild,    left,
            LAYOUT_AddChild,    right,
            TAG_END),
        TAG_END);
}

void ClipWindow_Open(void)
{
    struct Screen *scr;
    int i;

    if (Win) {
        IIntuition->WindowToFront(Win);
        IIntuition->ActivateWindow(Win);
        ClipWindow_Refresh();
        return;
    }
    for (i = 0; i < K_COUNT; i++) {
        if (!classes[i].base)
            classes[i].base = IIntuition->OpenClass(classes[i].name, 52,
                                                    &classes[i].cls);
        if (!classes[i].base)
            return;
    }
    for (i = 0; i < NUM_XFADE_KINDS; i++)
        KindLabels[i] = (STRPTR)FB_Str(XfadeKinds[i].msg);
    KindLabels[NUM_XFADE_KINDS] = NULL;
    /* In FBFit order: the project's, then each mode. */
    FitLabels[FB_FIT_DEFAULT] = (STRPTR)FB_Str(MSG_FIT_AS_PROJECT);
    FitLabels[FB_FIT_FIT]     = (STRPTR)FB_Str(MSG_MENU_FIT_FIT);
    FitLabels[FB_FIT_FILL]    = (STRPTR)FB_Str(MSG_MENU_FIT_FILL);
    FitLabels[FB_FIT_STRETCH] = (STRPTR)FB_Str(MSG_MENU_FIT_STRETCH);
    FitLabels[FB_FIT_COUNT]   = NULL;
    strcpy(NameText, FB_Str(MSG_CLIPWIN_NONE));
    SourceText[0] = ShownText[0] = EffectText[0] = '\0';
    EffectPos = 0;

    scr = IIntuition->LockPubScreen(NULL);
    if (!scr)
        return;
    WinObj = IIntuition->NewObject(CLS(K_WINDOW), NULL,
        WA_PubScreen,     scr,
        WA_Title,         FB_Str(MSG_CLIPWIN_TITLE),
        WA_Activate,      TRUE,
        WA_DepthGadget,   TRUE,
        WA_DragBar,       TRUE,
        WA_CloseGadget,   TRUE,
        WA_SizeGadget,    TRUE,
        WA_IDCMP,         IDCMP_GADGETUP | IDCMP_CLOSEWINDOW |
                          IDCMP_VANILLAKEY,
        WA_PointerType,   Working ? FB_WORKING_POINTER : POINTERTYPE_NORMAL,
        WINDOW_Position,  WPOS_CENTERMOUSE,
        WINDOW_Layout,    make_layout(),
        TAG_END);
    if (WinObj)
        Win = (struct Window *)IIntuition->IDoMethod(WinObj, WM_OPEN, NULL);
    IIntuition->UnlockPubScreen(NULL, scr);
    if (!Win) {
        if (WinObj)
            IIntuition->DisposeObject(WinObj);
        WinObj = NULL;
        return;
    }
    ClipWindow_Refresh();
}

void ClipWindow_SetWorking(BOOL working)
{
    Working = working;
    if (Win)
        IIntuition->SetWindowPointer(Win,
            WA_PointerType, working ? FB_WORKING_POINTER : POINTERTYPE_NORMAL,
            TAG_END);
}

uint32 ClipWindow_SigMask(void)
{
    uint32 mask = 0;

    if (WinObj && Win)
        IIntuition->GetAttr(WINDOW_SigMask, WinObj, &mask);
    return mask;
}

/* Closes the window but keeps the classes, to open it again. */
static void close_window(void)
{
    if (WinObj) {
        IIntuition->DisposeObject(WinObj);
        WinObj = NULL;
        Win = NULL;
    }
}

void ClipWindow_Handle(uint32 sigs)
{
    uint32 result;
    uint16 code;
    BOOL close = FALSE;

    if (!WinObj || !(sigs & ClipWindow_SigMask()))
        return;
    while ((result = IIntuition->IDoMethod(WinObj, WM_HANDLEINPUT, &code))
           != WMHI_LASTMSG) {
        switch (result & WMHI_CLASSMASK) {
        case WMHI_CLOSEWINDOW:
            close = TRUE;
            break;
        case WMHI_VANILLAKEY:
            if ((result & WMHI_KEYMASK) == 27)      /* Esc */
                close = TRUE;
            break;
        case WMHI_GADGETUP: {
            int clip = MainWindow_SelectedClip();

            if (clip >= 0)
                changed(result & WMHI_GADGETMASK, clip);
            break;
        }
        }
    }
    if (close)
        close_window();
}

void ClipWindow_Close(void)
{
    int i;

    close_window();
    for (i = K_COUNT - 1; i >= 0; i--) {
        if (classes[i].base)
            IIntuition->CloseClass(classes[i].base);
        classes[i].base = NULL;
    }
}
