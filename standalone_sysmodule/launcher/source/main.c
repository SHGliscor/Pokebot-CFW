#include <3ds.h>
#include <stdio.h>

#define POKEBOT_SYSMODULE_TID 0x000401300000F902ULL

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    printf("Pokebot3DS standalone sysmodule launcher\n\n");
    printf("This only starts the background module.\n");
    printf("HID remains UNPATCHED until the PC sends\n");
    printf("INPUT_ARM after ORAS is running.\n\n");

    Result res = nsInit();
    if(R_FAILED(res))
    {
        printf("nsInit failed: 0x%08lX\n", (unsigned long)res);
        printf("\nPress START to exit.\n");
        while(aptMainLoop())
        {
            hidScanInput();
            if(hidKeysDown() & KEY_START)
                break;
            gspWaitForVBlank();
        }
        gfxExit();
        return 1;
    }

    u32 pid = 0;
    res = NS_LaunchTitle(POKEBOT_SYSMODULE_TID, 0, &pid);
    nsExit();

    if(R_SUCCEEDED(res))
    {
        printf("Module launch requested successfully.\n");
        printf("PID: %lu\n\n", (unsigned long)pid);
        printf("Return to HOME, boot Alpha Sapphire,\n");
        printf("then run the PC smoke probe.\n");
    }
    else
    {
        printf("Launch failed: 0x%08lX\n\n", (unsigned long)res);
        printf("Check that:\n");
        printf("- 000401300000F902.cxi is in\n");
        printf("  /luma/sysmodules/\n");
        printf("- Luma external FIRMs/modules is ON\n");
    }

    printf("\nPress START to exit.\n");
    while(aptMainLoop())
    {
        hidScanInput();
        if(hidKeysDown() & KEY_START)
            break;
        gspWaitForVBlank();
    }

    gfxExit();
    return R_SUCCEEDED(res) ? 0 : 2;
}
