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
#include "formbrod/log.h"
#include "gui/mainwindow.h"
#include "gui/playwindow.h"
#include "arexx/arexx.h"
#include "prefs/prefs.h"
#include "locale/catalog.h"
#include "Formbrod_rev.h"

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

/* While tracing: which part of the loop runs, said when it changes. */
static void trace_branch(const char *what)
{
    static const char *last;

    if (what != last) {
        FB_TRACE("app", "main loop: %s", what);
        last = what;
    }
}

static const char USED ver[] = VERSTAG;

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

    FB_TRACE("app", "opening the main window");
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

            trace_branch("playing");
            sigs = IExec->SetSignal(0, mask) & mask;
            f = App_PollPlay(&sleep_ms);
            if (f)
                PlayWindow_Show(f);
            if (!App_Playing()) {
                FB_TRACE("app", "playing ended");
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
        } else if (App_StillBusy()) {
            /* The preview window follows the playhead: a turn of work on
             * the frame at it, on a backend of its own. An export goes on
             * alongside; proxies wait (a frame takes a fraction of a
             * second). */
            const FBPlayFrame *f;

            trace_branch("frame at the playhead");
            sigs = IExec->SetSignal(0, mask) & mask;
            f = App_PollStill();
            if (f && PlayWindow_IsOpen() && !App_Playing()) {
                char text[64];

                PlayWindow_Show(f);
                App_StillTimeText(text, sizeof(text));
                PlayWindow_SetText(text);
            }
            if (App.rendering) {
                FBProgress p = App_PollRender();

                MainWindow_SetProgress(&p);
                if (!App.rendering)
                    MainWindow_Refresh();
            }
        } else if (App.rendering) {
            FBProgress p;

            trace_branch("rendering");
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

            trace_branch("between preview windows");
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
            trace_branch("cache work");
            sigs = IExec->SetSignal(0, mask) & mask;
            if (App_PollCache())
                MainWindow_Refresh();
        } else if (App_CacheBusy()) {
            /* Just used: the window has the machine. Look again for
             * input every tick until it has been left alone a while. */
            trace_branch("cache waiting for the user to pause");
            sigs = IExec->SetSignal(0, mask) & mask;
            if (!sigs)
                IDOS->Delay(1);
        } else {
            trace_branch("waiting for events");
            sigs = IExec->Wait(mask);
        }

        /* The arrow with an hourglass while an export runs. Not for
         * proxies: they're made quietly whenever the window is idle. */
        MainWindow_SetWorking(App.rendering);

        if (sigs & SIGBREAKF_CTRL_C)
            App.quit = 1;
        /* The film strip missed thumbnails, or the lanes a waveform:
         * read them (small reads) and draw those again. */
        if (sigs & App_ThumbsSigMask()) {
            if (App_LoadThumbs())
                MainWindow_ThumbsLoaded();
            if (App_LoadWaves())
                MainWindow_WavesLoaded();
        }
        if (sigs & ARexx_SigMask()) {
            ARexx_Handle();
            MainWindow_Refresh();
        }
        if (sigs & MainWindow_SigMask()) {
            last_input = now_ms();
            MainWindow_Handle(sigs);
        }

        /* The preview window, while open and not playing, shows the
         * frame at the playhead, and a new one when the playhead moves
         * or the edit changes (or when it has just been opened). While
         * playing it shows playback, so the frame there is taken as
         * shown. */
        {
            static fb_ms_t shown_at = -1;
            static unsigned shown_gen;

            if (App_Playing()) {
                shown_at = MainWindow_Playhead();
                shown_gen = App.gen;
            } else if (!PlayWindow_IsOpen()) {
                shown_at = -1;
            } else if (MainWindow_Playhead() != shown_at
                       || App.gen != shown_gen) {
                shown_at = MainWindow_Playhead();
                shown_gen = App.gen;
                if (FB_TimelineDuration(&App.timeline) > 0)
                    App_StillShow(shown_at);
            }
        }

        /* Playing stopped (ended, failed or stopped) before its first
         * picture: the window still says it's being prepared; say how it
         * stopped instead (the status line). */
        {
            static int was_playing;

            if (was_playing && !App_Playing() && !PlayWindow_HasPicture())
                PlayWindow_SetMessage(App.status);
            was_playing = App_Playing();
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
