/*
 * main.c - startup and the main event loop.
 *
 * The loop waits on the window, the AppWindow port and the ARexx port.
 * While a render is running it polls the backend between events instead of
 * sleeping, so the backend can do its work in slices.
 */
#include <proto/exec.h>
#include <proto/dos.h>

#include "app.h"
#include "gui/mainwindow.h"
#include "gui/playwindow.h"
#include "arexx/arexx.h"
#include "prefs/prefs.h"
#include "locale/catalog.h"
#include "version.h"

/* Proxies are made only when the user has left the window alone this
 * long, so editing never waits for them. */
#define IDLE_MS 1500

/* Milliseconds by the system clock, in DOS ticks (20 ms). */
static uint32 now_ms(void)
{
    struct DateStamp ds;

    IDOS->DateStamp(&ds);
    return ((uint32)ds.ds_Days * 24 * 60 * 60 * TICKS_PER_SECOND +
            (uint32)ds.ds_Minute * 60 * TICKS_PER_SECOND +
            (uint32)ds.ds_Tick) * (1000 / TICKS_PER_SECOND);
}

static const char USED ver[] =
    "$VER: Formbrod " FB_VERSION " (" FB_VERSION_DATE ")";

int main(int argc, char **argv)
{
    int rc = RETURN_FAIL;
    int i;

    Locale_Open();          /* before App_Init(), which sets a status line */
    /* Program settings; without them nothing is remembered between runs,
     * which is no reason to stop. They come first because they say where
     * the log goes. */
    Prefs_Open();
    App_StartLog();
    if (App_Init() != 0)
        goto out;

    /* Files on the command line are added as clips; a project file is
     * opened instead. */
    for (i = 1; i < argc; i++) {
        if (App_IsProjectFile(argv[i]))
            App_OpenProject(argv[i]);
        else
            App_AddFile(argv[i]);
    }

    if (MainWindow_Open() != 0) {
        IDOS->Printf("%s", FB_Str(MSG_ERR_NO_WINDOW));
        goto out;
    }
    if (ARexx_Init() != 0)
        IDOS->Printf("%s", FB_Str(MSG_ERR_NO_AREXX));

    while (!App.quit) {
        uint32 mask = MainWindow_SigMask() | ARexx_SigMask() |
                      (uint32)App_ThumbsSigMask() | SIGBREAKF_CTRL_C;
        uint32 sigs;
        static uint32 last_input;

        if (App_Playing()) {
            /* Playing in the preview window: a turn of backend work, the
             * picture that's due, sound for AHI. The playhead and the time
             * under the picture follow four times a second (redrawing
             * the tracks is too much to do for every picture). */
            static long long last_ui;
            const FBPlayFrame *f;
            long sleep_ms;

            sigs = IExec->SetSignal(0, mask) & mask;
            f = App_PollPlay(&sleep_ms);
            if (f)
                PlayWindow_Show(f);
            if (!App_Playing()) {
                MainWindow_Refresh();           /* ended, or failed */
            } else {
                long long now = FB_BackendClockUs();

                if (now - last_ui >= 250000) {
                    char text[64];

                    last_ui = now;
                    App_PlayTimeText(text, sizeof(text));
                    PlayWindow_SetText(text);
                    MainWindow_SetPlayhead(App_PlayPosition());
                }
                if (!sigs && sleep_ms >= 20)
                    IDOS->Delay(1);             /* nothing due for a while */
            }
        } else if (App.rendering) {
            FBProgress p;

            sigs = IExec->SetSignal(0, mask) & mask;
            p = App_PollRender();
            MainWindow_SetProgress(&p);
            if (!App.rendering)
                MainWindow_Refresh();
        } else if (App_PreviewWaiting()) {
            /* Between preview windows: the next one starts when playback
             * has caught up (App_PollPreview). Proxies are made meanwhile
             * while the window is left alone, as below. */
            int was = App_Previewing();

            sigs = IExec->SetSignal(0, mask) & mask;
            App_PollPreview();
            if (App.rendering || was != App_Previewing())
                MainWindow_Refresh();     /* a window started, or it ended */
            else if (App_CacheBusy() && now_ms() - last_input >= IDLE_MS) {
                if (App_PollCache())
                    MainWindow_Refresh();
            } else if (!sigs)
                IDOS->Delay(App_CacheBusy() ? 1 : 5);   /* don't spin */
        } else if (App_CacheBusy() && now_ms() - last_input >= IDLE_MS) {
            /* Proxies are made in the background, a slice at a time,
             * between window and ARexx events, while nothing happens. */
            sigs = IExec->SetSignal(0, mask) & mask;
            if (App_PollCache())
                MainWindow_Refresh();
        } else if (App_CacheBusy()) {
            /* Just used: the window has the machine. Look again for
             * input every tick until it has been left alone a while. */
            sigs = IExec->SetSignal(0, mask) & mask;
            if (!sigs)
                IDOS->Delay(1);
        } else {
            sigs = IExec->Wait(mask);
        }

        /* The arrow with an hourglass while an export runs. Not for
         * proxies: they're made quietly whenever the window is idle. */
        MainWindow_SetWorking(App.rendering);

        if (sigs & SIGBREAKF_CTRL_C)
            App.quit = 1;
        /* The film strip missed thumbnails: read them (a few small
         * reads) and draw it again. */
        if (sigs & App_ThumbsSigMask()) {
            if (App_LoadThumbs())
                MainWindow_ThumbsLoaded();
        }
        if (sigs & ARexx_SigMask()) {
            ARexx_Handle();
            MainWindow_Refresh();
        }
        if (sigs & MainWindow_SigMask()) {
            last_input = now_ms();
            MainWindow_Handle(sigs);
        }
    }
    rc = RETURN_OK;

out:
    ARexx_Cleanup();
    MainWindow_Close();
    App_Cleanup();
    App_StopLog();
    Prefs_Close();
    Locale_Close();
    return rc;
}
