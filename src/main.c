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
#include "arexx/arexx.h"

static const char USED ver[] = "$VER: Formbrod 0.1 (09.11.2026)";

int main(int argc, char **argv)
{
    int rc = RETURN_FAIL;
    int i;

    if (App_Init() != 0)
        goto out;

    /* Files on the command line are added as clips. */
    for (i = 1; i < argc; i++)
        App_AddFile(argv[i]);

    if (MainWindow_Open() != 0) {
        IDOS->Printf("Formbrod: can't open window (are the ReAction classes installed?)\n");
        goto out;
    }
    if (ARexx_Init() != 0)
        IDOS->Printf("Formbrod: ARexx port not available\n");

    while (!App.quit) {
        uint32 mask = MainWindow_SigMask() | ARexx_SigMask() | SIGBREAKF_CTRL_C;
        uint32 sigs;

        if (App.rendering) {
            FBProgress p;

            sigs = IExec->SetSignal(0, mask) & mask;
            p = App_PollRender();
            MainWindow_SetProgress(&p);
            if (!App.rendering)
                MainWindow_Refresh();
        } else {
            sigs = IExec->Wait(mask);
        }

        if (sigs & SIGBREAKF_CTRL_C)
            App.quit = 1;
        if (sigs & ARexx_SigMask()) {
            ARexx_Handle();
            MainWindow_Refresh();
        }
        if (sigs & MainWindow_SigMask())
            MainWindow_Handle(sigs);
    }
    rc = RETURN_OK;

out:
    ARexx_Cleanup();
    MainWindow_Close();
    App_Cleanup();
    return rc;
}
