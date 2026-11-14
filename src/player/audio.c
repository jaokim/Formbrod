/*
 * audio.c - sound out through AHI (see audio.h).
 *
 * Not compiled against the SDK in the environment it was written in:
 * the AHI calls follow the AHI developer docs (device interface,
 * double-buffered CMD_WRITE with ahir_Link) and the OS4 exec object
 * allocation calls.
 */
#include <string.h>

#include <proto/exec.h>
#include <devices/ahi.h>

#include "formbrod/backend.h"   /* FB_PLAY_RATE */
#include "formbrod/log.h"
#include "player/audio.h"

#define CHUNK_FRAMES (FB_PLAY_RATE / 10)          /* 100 ms */
#define CHUNK_BYTES  (CHUNK_FRAMES * 2 * sizeof(short))

static struct MsgPort    *Port;
static struct AHIRequest *Req[2];
static short             *Buf[2];
static BOOL               Busy[2];
static BOOL               DeviceOpen;
static int                Next;        /* the request to send next */
static BOOL               Reported;    /* an AHI error was logged */

int Audio_Open(int unit)
{
    int i, err;

    if (DeviceOpen)
        return 0;
    Port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!Port)
        goto fail;
    Req[0] = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
                 ASOIOR_Size,      sizeof(struct AHIRequest),
                 ASOIOR_ReplyPort, Port,
                 TAG_END);
    if (!Req[0])
        goto fail;
    Req[0]->ahir_Version = 4;
    err = IExec->OpenDevice(AHINAME, unit, (struct IORequest *)Req[0], 0);
    if (err != 0) {
        /* Error codes from devices/ahi.h (AHIE_...) or exec's IOERR_. */
        FB_Log(FB_LOG_WARNING, "audio",
               "Can't open %s unit %d (error %d): no sound", AHINAME, unit,
               err);
        goto fail;
    }
    DeviceOpen = TRUE;
    Req[1] = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
                 ASOIOR_Duplicate, Req[0],
                 TAG_END);
    if (!Req[1])
        goto fail;
    for (i = 0; i < 2; i++) {
        Buf[i] = IExec->AllocVecTags(CHUNK_BYTES, AVT_Type, MEMF_SHARED,
                                     AVT_ClearWithValue, 0, TAG_END);
        if (!Buf[i])
            goto fail;
        Busy[i] = FALSE;
    }
    Next = 0;
    Reported = FALSE;
    FB_Log(FB_LOG_INFO, "audio", "Sound through %s unit %d, %d Hz", AHINAME,
           unit, FB_PLAY_RATE);
    return 0;

fail:
    Audio_Close();
    return -1;
}

/* Collects requests AHI has finished with. */
static void collect(void)
{
    int i;

    for (i = 0; i < 2; i++) {
        if (Busy[i] && IExec->CheckIO((struct IORequest *)Req[i])) {
            IExec->WaitIO((struct IORequest *)Req[i]);
            Busy[i] = FALSE;
            /* A request AHI refused (a sample type or rate it won't
             * play, say) is silence; say why, once. */
            if (Req[i]->ahir_Std.io_Error != 0 && !Reported) {
                FB_Log(FB_LOG_WARNING, "audio", "AHI refused the sound "
                       "(error %d)", Req[i]->ahir_Std.io_Error);
                Reported = TRUE;
            }
        }
    }
}

void Audio_Feed(AudioFill fill, void *ud)
{
    if (!DeviceOpen)
        return;
    collect();
    /* Send in turn, each linked to the one still playing, if any. */
    while (!Busy[Next]) {
        struct AHIRequest *r = Req[Next];
        struct AHIRequest *prev = Busy[Next ^ 1] ? Req[Next ^ 1] : NULL;

        fill(ud, Buf[Next], CHUNK_FRAMES);
        r->ahir_Std.io_Message.mn_Node.ln_Pri = 0;
        r->ahir_Std.io_Command = CMD_WRITE;
        r->ahir_Std.io_Data    = Buf[Next];
        r->ahir_Std.io_Length  = CHUNK_BYTES;
        r->ahir_Std.io_Offset  = 0;
        r->ahir_Frequency      = FB_PLAY_RATE;
        r->ahir_Type           = AHIST_S16S;
        r->ahir_Volume         = 0x10000;     /* full */
        r->ahir_Position       = 0x8000;      /* centre */
        r->ahir_Link           = prev;
        IExec->SendIO((struct IORequest *)r);
        Busy[Next] = TRUE;
        Next ^= 1;
    }
}

void Audio_Stop(void)
{
    int i;

    for (i = 0; i < 2; i++) {
        if (Busy[i]) {
            IExec->AbortIO((struct IORequest *)Req[i]);
            IExec->WaitIO((struct IORequest *)Req[i]);
            Busy[i] = FALSE;
        }
    }
    Next = 0;
}

void Audio_Close(void)
{
    int i;

    if (DeviceOpen)
        Audio_Stop();
    for (i = 0; i < 2; i++) {
        if (Buf[i])
            IExec->FreeVec(Buf[i]);
        Buf[i] = NULL;
    }
    if (Req[1])
        IExec->FreeSysObject(ASOT_IOREQUEST, Req[1]);
    Req[1] = NULL;
    if (DeviceOpen)
        IExec->CloseDevice((struct IORequest *)Req[0]);
    DeviceOpen = FALSE;
    if (Req[0])
        IExec->FreeSysObject(ASOT_IOREQUEST, Req[0]);
    Req[0] = NULL;
    if (Port)
        IExec->FreeSysObject(ASOT_PORT, Port);
    Port = NULL;
}
