/*
 * projectwindow.c - the project settings window: the frame the edit is
 * made in, and how it's exported.
 *
 * Opened with Project > Project settings. Like the clip window, every
 * change goes through an App_* action (as ARexx's FORMAT and
 * EXPORTSETTINGS do) when a field is left with Return or Tab, or a
 * chooser is picked, and everything is saved in the project file.
 *
 * Groups:
 *   Frame        Width and Height (empty: the first clip's), Frame rate,
 *                Fit (the default for clips) and Bar colour
 *   Export file  File type: the suffix the export requester suggests
 *   Picture      Encoder and Quality
 *   Sound        Encoder, Bit rate and Sample rate
 *
 * The choosers offer the usual choices. Encoders this build of ffmpeg
 * lacks say so (export then falls back to one the file type can hold). A
 * value set from ARexx that isn't among the choices gets an entry of its
 * own, so the window never shows something other than what is set.
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
#include "formbrod/log.h"
#include "locale/catalog.h"
#include "gui/projectwindow.h"
#include "gui/mainwindow.h"
#include "gui/winprefs.h"

enum {
    PG_WIDTH = 1, PG_HEIGHT, PG_HINT, PG_RATE, PG_FIT, PG_PAD,
    PG_TYPE,
    PG_VCODEC, PG_QUALITY,
    PG_ACODEC, PG_BITRATE, PG_ARATE,
    PG_LAST
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
static struct Gadget *Gad[PG_LAST];
static BOOL Working;

/* ---- The choices ------------------------------------------------------- */

typedef struct { const char *name; int msg; } Named;

static const Named VCodecs[] = {
    { "mpeg4",   MSG_VCODEC_MPEG4 },
    { "libx264", MSG_VCODEC_H264 },
    { "mjpeg",   MSG_VCODEC_MJPEG },
};
static const Named ACodecs[] = {
    { "aac",        MSG_ACODEC_AAC },
    { "libmp3lame", MSG_ACODEC_MP3 },
    { "mp2",        MSG_ACODEC_MP2 },
    { "flac",       MSG_ACODEC_FLAC },
    { "pcm_s16le",  MSG_ACODEC_PCM },
};
static const Named Types[] = {
    { "mp4", MSG_TYPE_MP4 },
    { "mov", MSG_TYPE_MOV },
    { "mkv", MSG_TYPE_MKV },
    { "avi", MSG_TYPE_AVI },
};
static const int Qualities[] = {
    MSG_QUALITY_LOW, MSG_QUALITY_MEDIUM, MSG_QUALITY_HIGH, MSG_QUALITY_BEST
};
static const int Bitrates[] = { 96, 128, 160, 192, 256, 320 };
static const int Arates[] = { 44100, 48000 };
/* The first is "as the first clip" (0/0). */
static const struct { int num, den; } Rates[] = {
    { 0, 0 }, { 24000, 1001 }, { 24, 1 }, { 25, 1 }, { 30000, 1001 },
    { 30, 1 }, { 50, 1 }, { 60000, 1001 }, { 60, 1 },
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
/* Room for every choice, one more of the project's own, and the NULL. */
#define MAX_CHOICES 12

/* A chooser's labels and the texts they point at. `extra` is the index of
 * the entry for a value that isn't among the choices, or -1. */
typedef struct {
    STRPTR labels[MAX_CHOICES];
    char   text[MAX_CHOICES][64];
    int    count, extra;
    char   extra_key[32];        /* the value `extra` stands for */
} Choices;

static Choices VCodecCh, ACodecCh, TypeCh, QualityCh, BitrateCh, ArateCh,
               RateCh, FitCh;

static void ch_reset(Choices *c)
{
    c->count = 0;
    c->extra = -1;
    c->extra_key[0] = '\0';
    c->labels[0] = NULL;
}

/* Whether a chooser has an entry for a value: one of the choices
 * (found >= 0) or its extra one. */
static BOOL ch_covers(const Choices *c, int found, const char *key)
{
    return found >= 0 || (c->extra >= 0 && strcmp(c->extra_key, key) == 0);
}

/* The keys of the numbers, for the extra entries. */
static const char *int_key(int v)
{
    static char buf[16];

    snprintf(buf, sizeof(buf), "%d", v);
    return buf;
}

static const char *rate_key(int num, int den)
{
    static char buf[32];

    snprintf(buf, sizeof(buf), "%d/%d", num, den);
    return buf;
}

static void ch_add(Choices *c, const char *text)
{
    if (c->count >= MAX_CHOICES - 1)
        return;
    snprintf(c->text[c->count], sizeof(c->text[0]), "%s", text);
    c->labels[c->count] = (STRPTR)c->text[c->count];
    c->labels[++c->count] = NULL;
}

/* Adds the entry for a value that isn't among the choices. */
static void ch_add_extra(Choices *c, const char *text, const char *key)
{
    c->extra = c->count;
    snprintf(c->extra_key, sizeof(c->extra_key), "%s", key);
    ch_add(c, text);
}

/* An encoder's entry: its label, and whether ffmpeg has it. */
static void ch_add_encoder(Choices *c, const Named *n)
{
    char buf[64];

    if (FB_HaveEncoder(n->name) == 0) {
        snprintf(buf, sizeof(buf), FB_Str(MSG_NOT_AVAILABLE),
                 FB_Str(n->msg));
        ch_add(c, buf);
    } else {
        ch_add(c, FB_Str(n->msg));
    }
}

/* A number with the locale's decimal point and at most `decimals`
 * decimals, without trailing zeros: "23,976", "25", "44,1". */
static void format_decimal(char *buf, size_t len, double v, int decimals)
{
    char dp = Locale_DecimalPoint(), *p;
    size_t n;

    snprintf(buf, len, "%.*f", decimals, v);
    if ((p = strchr(buf, '.')) != NULL) {
        n = strlen(buf);
        while (n > 0 && buf[n - 1] == '0')
            buf[--n] = '\0';
        if (n > 0 && buf[n - 1] == '.')
            buf[--n] = '\0';
        else
            *p = dp;
    }
}

static void rate_text(char *buf, size_t len, int num, int den)
{
    char n[24];

    if (num <= 0 || den <= 0) {
        snprintf(buf, len, "%s", FB_Str(MSG_RATE_AUTO));
        return;
    }
    format_decimal(n, sizeof(n), (double)num / den, 3);
    snprintf(buf, len, FB_Str(MSG_RATE_FPS), n);
}

static void arate_text(char *buf, size_t len, int hz)
{
    char n[24];

    format_decimal(n, sizeof(n), hz / 1000.0, 3);
    snprintf(buf, len, FB_Str(MSG_KHZ), n);
}

static int find_named(const Named *list, int n, const char *name)
{
    int i;

    for (i = 0; i < n; i++)
        if (strcmp(list[i].name, name) == 0)
            return i;
    return -1;
}

static int find_int(const int *list, int n, int v)
{
    int i;

    for (i = 0; i < n; i++)
        if (list[i] == v)
            return i;
    return -1;
}

static int find_rate(int num, int den)
{
    int i;

    for (i = 0; i < COUNT(Rates); i++)
        if ((long)Rates[i].num * (den ? den : 1) ==
            (long)num * (Rates[i].den ? Rates[i].den : 1) &&
            (Rates[i].num == 0) == (num == 0))
            return i;
    return -1;
}

/* Builds every chooser's labels for the project as it is now: the
 * choices, and an entry of its own for a value that isn't one. */
static void make_choices(void)
{
    const FBFormat *f = &App.timeline.format;
    const FBExport *e = &f->encode;
    char buf[64];
    int i;

    ch_reset(&VCodecCh);
    for (i = 0; i < COUNT(VCodecs); i++)
        ch_add_encoder(&VCodecCh, &VCodecs[i]);
    if (find_named(VCodecs, COUNT(VCodecs), e->vcodec) < 0)
        ch_add_extra(&VCodecCh, e->vcodec, e->vcodec);  /* ffmpeg's name */

    ch_reset(&ACodecCh);
    for (i = 0; i < COUNT(ACodecs); i++)
        ch_add_encoder(&ACodecCh, &ACodecs[i]);
    if (find_named(ACodecs, COUNT(ACodecs), e->acodec) < 0)
        ch_add_extra(&ACodecCh, e->acodec, e->acodec);

    ch_reset(&TypeCh);
    for (i = 0; i < COUNT(Types); i++)
        ch_add(&TypeCh, FB_Str(Types[i].msg));
    if (find_named(Types, COUNT(Types), e->filetype) < 0) {
        snprintf(buf, sizeof(buf), ".%s", e->filetype);
        ch_add_extra(&TypeCh, buf, e->filetype);
    }

    ch_reset(&QualityCh);
    for (i = 0; i < COUNT(Qualities); i++)
        ch_add(&QualityCh, FB_Str(Qualities[i]));

    ch_reset(&BitrateCh);
    for (i = 0; i < COUNT(Bitrates); i++) {
        snprintf(buf, sizeof(buf), FB_Str(MSG_KBITS), (long)Bitrates[i]);
        ch_add(&BitrateCh, buf);
    }
    if (find_int(Bitrates, COUNT(Bitrates), e->abitrate) < 0) {
        snprintf(buf, sizeof(buf), FB_Str(MSG_KBITS), (long)e->abitrate);
        ch_add_extra(&BitrateCh, buf, int_key(e->abitrate));
    }

    ch_reset(&ArateCh);
    for (i = 0; i < COUNT(Arates); i++) {
        arate_text(buf, sizeof(buf), Arates[i]);
        ch_add(&ArateCh, buf);
    }
    if (find_int(Arates, COUNT(Arates), e->arate) < 0) {
        arate_text(buf, sizeof(buf), e->arate);
        ch_add_extra(&ArateCh, buf, int_key(e->arate));
    }

    ch_reset(&RateCh);
    for (i = 0; i < COUNT(Rates); i++) {
        rate_text(buf, sizeof(buf), Rates[i].num, Rates[i].den);
        ch_add(&RateCh, buf);
    }
    if (find_rate(f->fps_num, f->fps_den) < 0) {
        rate_text(buf, sizeof(buf), f->fps_num, f->fps_den);
        ch_add_extra(&RateCh, buf, rate_key(f->fps_num, f->fps_den));
    }

    /* In FBFit order, without DEFAULT (the project's is never that). */
    ch_reset(&FitCh);
    ch_add(&FitCh, FB_Str(MSG_MENU_FIT_FIT));
    ch_add(&FitCh, FB_Str(MSG_MENU_FIT_FILL));
    ch_add(&FitCh, FB_Str(MSG_MENU_FIT_STRETCH));
}

/* Whether the choosers, made when the window opened, still have an entry
 * for everything set (ARexx may have set something new since). */
static BOOL choices_cover(void)
{
    const FBFormat *f = &App.timeline.format;
    const FBExport *e = &f->encode;

    return ch_covers(&VCodecCh, find_named(VCodecs, COUNT(VCodecs),
                                           e->vcodec), e->vcodec) &&
           ch_covers(&ACodecCh, find_named(ACodecs, COUNT(ACodecs),
                                           e->acodec), e->acodec) &&
           ch_covers(&TypeCh, find_named(Types, COUNT(Types), e->filetype),
                     e->filetype) &&
           ch_covers(&BitrateCh, find_int(Bitrates, COUNT(Bitrates),
                                          e->abitrate), int_key(e->abitrate)) &&
           ch_covers(&ArateCh, find_int(Arates, COUNT(Arates), e->arate),
                     int_key(e->arate)) &&
           ch_covers(&RateCh, find_rate(f->fps_num, f->fps_den),
                     rate_key(f->fps_num, f->fps_den));
}

/* ---- Making the gadgets ------------------------------------------------ */

static char FieldText[PG_LAST][24];

static Object *label(const char *text)
{
    return IIntuition->NewObject(CLS(K_LABEL), NULL, LABEL_Text, text,
                                 TAG_END);
}

static Object *field(uint32 id, int max)
{
    return (Object *)(Gad[id] = (struct Gadget *)IIntuition->NewObject(
            CLS(K_STRING), NULL,
        GA_ID,            id,
        GA_RelVerify,     TRUE,
        GA_TabCycle,      TRUE,
        STRINGA_MaxChars, max,
        STRINGA_TextVal,  "",
        TAG_END));
}

static Object *text(uint32 id, const char *t)
{
    return (Object *)(Gad[id] = (struct Gadget *)IIntuition->NewObject(
            CLS(K_BUTTON), NULL,
        GA_ID,       id,
        GA_ReadOnly, TRUE,
        GA_Text,     t,
        BUTTON_Justification, BCJ_LEFT,
        TAG_END));
}

static Object *chooser(uint32 id, Choices *c)
{
    return (Object *)(Gad[id] = (struct Gadget *)IIntuition->NewObject(
            CLS(K_CHOOSER), NULL,
        GA_ID,              id,
        GA_RelVerify,       TRUE,
        CHOOSER_LabelArray, c->labels,
        CHOOSER_Selected,   0,
        TAG_END));
}

static Object *make_layout(void)
{
    Object *frame, *file, *picture, *sound, *left, *right;

    frame = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_BevelStyle,  BVS_GROUP,
        LAYOUT_Label,       FB_Str(MSG_GROUP_FRAME),
        LAYOUT_AddChild,    field(PG_WIDTH, 6),
        CHILD_Label,        label(FB_Str(MSG_LABEL_WIDTH)),
        LAYOUT_AddChild,    field(PG_HEIGHT, 6),
        CHILD_Label,        label(FB_Str(MSG_LABEL_HEIGHT)),
        LAYOUT_AddChild,    text(PG_HINT, FB_Str(MSG_HINT_SIZE_AUTO)),
        LAYOUT_AddChild,    chooser(PG_RATE, &RateCh),
        CHILD_Label,        label(FB_Str(MSG_LABEL_FRAME_RATE)),
        LAYOUT_AddChild,    chooser(PG_FIT, &FitCh),
        CHILD_Label,        label(FB_Str(MSG_LABEL_FIT)),
        LAYOUT_AddChild,    field(PG_PAD, 7),
        CHILD_Label,        label(FB_Str(MSG_LABEL_BAR_COLOUR)),
        TAG_END);

    file = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_BevelStyle,  BVS_GROUP,
        LAYOUT_Label,       FB_Str(MSG_GROUP_EXPORT_FILE),
        LAYOUT_AddChild,    chooser(PG_TYPE, &TypeCh),
        CHILD_Label,        label(FB_Str(MSG_LABEL_FILE_TYPE)),
        TAG_END);

    picture = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_BevelStyle,  BVS_GROUP,
        LAYOUT_Label,       FB_Str(MSG_GROUP_PICTURE),
        LAYOUT_AddChild,    chooser(PG_VCODEC, &VCodecCh),
        CHILD_Label,        label(FB_Str(MSG_LABEL_ENCODER)),
        LAYOUT_AddChild,    chooser(PG_QUALITY, &QualityCh),
        CHILD_Label,        label(FB_Str(MSG_LABEL_QUALITY)),
        TAG_END);

    sound = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_BevelStyle,  BVS_GROUP,
        LAYOUT_Label,       FB_Str(MSG_GROUP_SOUND),
        LAYOUT_AddChild,    chooser(PG_ACODEC, &ACodecCh),
        CHILD_Label,        label(FB_Str(MSG_LABEL_ENCODER)),
        LAYOUT_AddChild,    chooser(PG_BITRATE, &BitrateCh),
        CHILD_Label,        label(FB_Str(MSG_LABEL_BITRATE)),
        LAYOUT_AddChild,    chooser(PG_ARATE, &ArateCh),
        CHILD_Label,        label(FB_Str(MSG_LABEL_SAMPLE_RATE)),
        TAG_END);

    left = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_AddChild,    frame,
        CHILD_WeightedHeight, 0,
        LAYOUT_AddChild,    file,
        CHILD_WeightedHeight, 0,
        TAG_END);
    right = IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_AddChild,    picture,
        CHILD_WeightedHeight, 0,
        LAYOUT_AddChild,    sound,
        CHILD_WeightedHeight, 0,
        TAG_END);

    return IIntuition->NewObject(CLS(K_LAYOUT), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
        LAYOUT_SpaceOuter,  TRUE,
        LAYOUT_AddChild,    left,
        LAYOUT_AddChild,    right,
        TAG_END);
}

/* ---- Showing the project ------------------------------------------------ */

static void set_gad(uint32 id, uint32 tag, uint32 value, BOOL disabled)
{
    if (IIntuition->SetGadgetAttrs(Gad[id], Win, NULL,
                                   tag,         value,
                                   GA_Disabled, disabled,
                                   TAG_END) && Win)
        IIntuition->RefreshGList(Gad[id], Win, NULL, 1);
}

static void set_field(uint32 id)
{
    set_gad(id, STRINGA_TextVal, (uint32)FieldText[id], FALSE);
}

static void set_choice(uint32 id, int sel, Choices *c, BOOL disabled)
{
    if (sel < 0)
        sel = c->extra >= 0 ? c->extra : 0;
    set_gad(id, CHOOSER_Selected, (uint32)sel, disabled);
}

static void open_window(void);
static void close_window(void);

void ProjectWindow_Refresh(void)
{
    const FBFormat *f = &App.timeline.format;
    const FBExport *e = &f->encode;

    if (!WinObj || !Win)
        return;
    /* A value the choosers have no entry for: make them again. */
    if (!choices_cover()) {
        close_window();
        open_window();
        return;
    }

    if (f->width > 0 && f->height > 0) {
        snprintf(FieldText[PG_WIDTH], sizeof(FieldText[0]), "%d", f->width);
        snprintf(FieldText[PG_HEIGHT], sizeof(FieldText[0]), "%d", f->height);
    } else {
        FieldText[PG_WIDTH][0] = FieldText[PG_HEIGHT][0] = '\0';
    }
    set_field(PG_WIDTH);
    set_field(PG_HEIGHT);
    snprintf(FieldText[PG_PAD], sizeof(FieldText[0]), "%06lX",
             (unsigned long)(f->pad_color & 0xFFFFFF));
    set_field(PG_PAD);
    set_choice(PG_RATE, find_rate(f->fps_num, f->fps_den), &RateCh, FALSE);
    set_choice(PG_FIT, f->fit > FB_FIT_DEFAULT ? (int)f->fit - 1 : 0, &FitCh,
               FALSE);

    set_choice(PG_TYPE, find_named(Types, COUNT(Types), e->filetype),
               &TypeCh, FALSE);
    set_choice(PG_VCODEC, find_named(VCodecs, COUNT(VCodecs), e->vcodec),
               &VCodecCh, FALSE);
    set_choice(PG_QUALITY, e->vquality - FB_VQ_LOW, &QualityCh, FALSE);
    set_choice(PG_ACODEC, find_named(ACodecs, COUNT(ACodecs), e->acodec),
               &ACodecCh, FALSE);
    /* PCM and FLAC have no bit rate. */
    set_choice(PG_BITRATE, find_int(Bitrates, COUNT(Bitrates), e->abitrate),
               &BitrateCh, FB_EncoderLossless(e->acodec));
    set_choice(PG_ARATE, find_int(Arates, COUNT(Arates), e->arate),
               &ArateCh, FALSE);
}

/* ---- Changes ---------------------------------------------------------- */

static const char *field_text(uint32 id)
{
    STRPTR t = NULL;

    IIntuition->GetAttr(STRINGA_TextVal, (Object *)Gad[id], (uint32 *)&t);
    return t ? (const char *)t : "";
}

/* A whole number typed in a field: 0 if it's empty, -1 if it isn't one. */
static long field_number(uint32 id)
{
    const char *t = field_text(id);
    char *end;
    long v;

    while (*t == ' ')
        t++;
    if (!*t)
        return 0;
    v = strtol(t, &end, 10);
    while (*end == ' ')
        end++;
    if (*end || v < 0) {
        App_SetStatus(FB_Str(MSG_STATUS_BAD_NUMBER), field_text(id));
        return -1;
    }
    return v;
}

static void changed(uint32 id)
{
    FBExport e = App.timeline.format.encode;
    uint32 sel = 0;
    int export_change = 0;

    if (id != PG_WIDTH && id != PG_HEIGHT && id != PG_PAD)
        IIntuition->GetAttr(CHOOSER_Selected, (Object *)Gad[id], &sel);

    switch (id) {
    case PG_WIDTH:
    case PG_HEIGHT: {
        long w = field_number(PG_WIDTH), h = field_number(PG_HEIGHT);

        if (w < 0 || h < 0)
            break;
        /* Both empty: the first clip's. One of them: wait for the other. */
        if ((w == 0) != (h == 0))
            break;
        if (App_SetFrameSize((int)w, (int)h) != 0)
            App_SetStatus("%s", FB_Str(MSG_STATUS_BAD_SIZE));
        break;
    }
    case PG_PAD: {
        const char *t = field_text(PG_PAD);
        char *end;
        unsigned long rgb = strtoul(t[0] == '#' ? t + 1 : t, &end, 16);

        if (!*t || *end || App_SetPadColor(rgb) != 0)
            App_SetStatus(FB_Str(MSG_STATUS_BAD_COLOUR), t);
        break;
    }
    case PG_RATE:
        if ((int)sel < COUNT(Rates))
            App_SetFrameRate(Rates[sel].num, Rates[sel].den);
        break;
    case PG_FIT:
        App_SetDefaultFit((int)sel + 1);    /* FB_FIT_FIT is 1 */
        break;
    case PG_TYPE:
        if ((int)sel < COUNT(Types)) {
            snprintf(e.filetype, sizeof(e.filetype), "%s", Types[sel].name);
            export_change = 1;
        }
        break;
    case PG_VCODEC:
        if ((int)sel < COUNT(VCodecs)) {
            snprintf(e.vcodec, sizeof(e.vcodec), "%s", VCodecs[sel].name);
            export_change = 1;
        }
        break;
    case PG_QUALITY:
        e.vquality = FB_VQ_LOW + (int)sel;
        export_change = 1;
        break;
    case PG_ACODEC:
        if ((int)sel < COUNT(ACodecs)) {
            snprintf(e.acodec, sizeof(e.acodec), "%s", ACodecs[sel].name);
            export_change = 1;
        }
        break;
    case PG_BITRATE:
        if ((int)sel < COUNT(Bitrates)) {
            e.abitrate = Bitrates[sel];
            export_change = 1;
        }
        break;
    case PG_ARATE:
        if ((int)sel < COUNT(Arates)) {
            e.arate = Arates[sel];
            export_change = 1;
        }
        break;
    }
    if (export_change)
        App_SetExport(&e);
    MainWindow_Refresh();       /* which shows it here too */
}

/* ---- The window -------------------------------------------------------- */

static void open_window(void)
{
    struct Screen *scr;
    struct TagItem place[5];
    BOOL placed;

    make_choices();
    scr = IIntuition->LockPubScreen(NULL);
    if (!scr) {
        FB_Log(FB_LOG_WARNING, "gui", "Project window: can't lock the "
               "default public screen");
        return;
    }
    placed = WinPrefs_Tags("ProjectWindow", scr, place);
    WinObj = IIntuition->NewObject(CLS(K_WINDOW), NULL,
        WA_PubScreen,     scr,
        WA_Title,         FB_Str(MSG_PROJWIN_TITLE),
        WA_Activate,      TRUE,
        WA_DepthGadget,   TRUE,
        WA_DragBar,       TRUE,
        WA_CloseGadget,   TRUE,
        WA_SizeGadget,    TRUE,
        WA_IDCMP,         IDCMP_GADGETUP | IDCMP_CLOSEWINDOW |
                          IDCMP_VANILLAKEY,
        WA_PointerType,   Working ? FB_WORKING_POINTER : POINTERTYPE_NORMAL,
        placed ? TAG_IGNORE : WINDOW_Position, WPOS_CENTERMOUSE,
        WINDOW_Layout,    make_layout(),
        TAG_MORE,         place);
    if (WinObj)
        Win = (struct Window *)IIntuition->IDoMethod(WinObj, WM_OPEN, NULL);
    IIntuition->UnlockPubScreen(NULL, scr);
    if (!Win) {
        FB_Log(FB_LOG_WARNING, "gui", "Project window: %s",
               WinObj ? "WM_OPEN failed" : "can't make the window object");
        if (WinObj)
            IIntuition->DisposeObject(WinObj);
        WinObj = NULL;
        return;
    }
    ProjectWindow_Refresh();
}

void ProjectWindow_Open(void)
{
    int i;

    FB_ENTER("gui");
    if (Win) {
        IIntuition->WindowToFront(Win);
        IIntuition->ActivateWindow(Win);
        ProjectWindow_Refresh();
        FB_LEAVE("gui", "already open: to front");
        return;
    }
    for (i = 0; i < K_COUNT; i++) {
        if (!classes[i].base)
            classes[i].base = IIntuition->OpenClass(classes[i].name, 52,
                                                    &classes[i].cls);
        if (!classes[i].base) {
            FB_Log(FB_LOG_WARNING, "gui", "Project window: can't open %s "
                   "(version 52)", classes[i].name);
            FB_LEAVE("gui", "no %s", classes[i].name);
            return;
        }
    }
    open_window();
    FB_LEAVE("gui", "window %p", (void *)Win);
}

void ProjectWindow_SetWorking(BOOL working)
{
    Working = working;
    if (Win)
        IIntuition->SetWindowPointer(Win,
            WA_PointerType, working ? FB_WORKING_POINTER : POINTERTYPE_NORMAL,
            TAG_END);
}

uint32 ProjectWindow_SigMask(void)
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
        WinPrefs_Save("ProjectWindow", Win);
        IIntuition->DisposeObject(WinObj);
        WinObj = NULL;
        Win = NULL;
        memset(Gad, 0, sizeof(Gad));
    }
}

void ProjectWindow_Handle(uint32 sigs)
{
    uint32 result;
    uint16 code;
    BOOL close = FALSE;
    uint32 changes[PG_LAST];
    int n = 0, i;

    if (!WinObj || !(sigs & ProjectWindow_SigMask()))
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
        case WMHI_GADGETUP:
            /* Applied after the loop: a change may make the window
             * again (a new choice), which this loop must not see. */
            if (n < PG_LAST)
                changes[n++] = result & WMHI_GADGETMASK;
            break;
        }
    }
    for (i = 0; i < n && WinObj; i++)
        changed(changes[i]);
    if (close)
        close_window();
}

void ProjectWindow_Close(void)
{
    int i;

    close_window();
    for (i = K_COUNT - 1; i >= 0; i--) {
        if (classes[i].base)
            IIntuition->CloseClass(classes[i].base);
        classes[i].base = NULL;
    }
}
