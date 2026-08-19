/*
 * Pokebot3DS HID-only controller backend.
 *
 * Uses the same HID hook routine as Luma InputRedirection, but with a private
 * remote-input buffer and without starting UDP/4950 or touching IR. This lets
 * the Pokebot bridge own standard buttons/touch/circle-pad independently while
 * legacy InputRedirection remains disabled.
 */

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "input_redirection.h"
#include "memory.h"
#include "pokebot_input.h"
#include "process_patches.h"

extern void hidCodePatchFunc(void);

static const u32 g_hidOrigRegisterAndValue[] = {0x1EC46000, 0x4001};
static const u32 g_hidOrigCode[] = {
    0xE92D4070, // push {r4-r6, lr}
    0xE1A05001, // mov  r5, r1
    0xEE1D4F70, // mrc  p15, 0, r4, c13, c0, 3
    0xE3A01801, // mov  r1, #0x10000
    0xE5A41080, // str  r1, [r4,#0x80]!
};

//                       local hid, local tsrd, local cprd, local tswr, local cpwr, remote hid, remote ts, remote circle
static u32 g_hidData[] = {0x00000FFF, 0x02000000, 0x007FF7FF, 0x00000000,
                          0x00000000, 0x00000FFF, 0x02000000, 0x007FF7FF};

static bool g_enabled = false;
static bool g_patchPrepared = false;
static u32 *g_hidRegPatchOffsets[2] = {NULL, NULL};
static u32 *g_hidPatchJumpLoc = NULL;

static Result doHidPatch(Handle processHandle, bool enable)
{
    s64 startAddress = 0;
    s64 textTotalRoundedSize = 0;
    s64 rodataTotalRoundedSize = 0;
    s64 dataTotalRoundedSize = 0;

    svcGetProcessInfo(&textTotalRoundedSize, processHandle, 0x10002);
    svcGetProcessInfo(&rodataTotalRoundedSize, processHandle, 0x10003);
    svcGetProcessInfo(&dataTotalRoundedSize, processHandle, 0x10004);

    const u32 totalSize = (u32)(textTotalRoundedSize + rodataTotalRoundedSize + dataTotalRoundedSize);
    svcGetProcessInfo(&startAddress, processHandle, 0x10005);

    Result res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, 0x00100000, processHandle,
                                       (u32)startAddress, totalSize, 0);
    if(R_FAILED(res))
        return res;

    if(!g_patchPrepared)
    {
        u32 *off = (u32 *)memsearch((u8 *)0x00100000, g_hidOrigRegisterAndValue,
                                    totalSize, sizeof(g_hidOrigRegisterAndValue));
        if(off == NULL)
        {
            svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, 0x00100000, totalSize);
            return -1;
        }

        const u32 used = (u32)off - 0x00100000u;
        u32 *off2 = (u32 *)memsearch((u8 *)off + sizeof(g_hidOrigRegisterAndValue),
                                     g_hidOrigRegisterAndValue,
                                     totalSize - used,
                                     sizeof(g_hidOrigRegisterAndValue));
        if(off2 == NULL)
        {
            svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, 0x00100000, totalSize);
            return -2;
        }

        u32 *off3 = (u32 *)memsearch((u8 *)0x00100000, g_hidOrigCode,
                                     totalSize, sizeof(g_hidOrigCode));
        if(off3 == NULL)
        {
            svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, 0x00100000, totalSize);
            return -3;
        }

        g_hidRegPatchOffsets[0] = off;
        g_hidRegPatchOffsets[1] = off2;
        g_hidPatchJumpLoc = off3;
        g_patchPrepared = true;
    }

    if(enable)
    {
        const u32 hidDataPhys = (u32)PA_FROM_VA_PTR(g_hidData);
        const u32 hidCodePhys = (u32)PA_FROM_VA_PTR(&hidCodePatchFunc);
        const u32 hidHook[] = {
            0xE59F3004, // ldr r3,  [pc, #4]
            0xE59FC004, // ldr r12, [pc, #4]
            0xE12FFF1C, // bx  r12
            hidDataPhys,
            hidCodePhys,
        };

        *g_hidRegPatchOffsets[0] = *g_hidRegPatchOffsets[1] = hidDataPhys;
        memcpy(g_hidPatchJumpLoc, hidHook, sizeof(hidHook));
    }
    else
    {
        memcpy(g_hidRegPatchOffsets[0], g_hidOrigRegisterAndValue,
               sizeof(g_hidOrigRegisterAndValue));
        memcpy(g_hidRegPatchOffsets[1], g_hidOrigRegisterAndValue,
               sizeof(g_hidOrigRegisterAndValue));
        memcpy(g_hidPatchJumpLoc, g_hidOrigCode, sizeof(g_hidOrigCode));
    }

    svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, 0x00100000, totalSize);
    return 0;
}

void PokebotInput_ReleaseAll(void)
{
    u32 *phys = PA_FROM_VA_PTR(g_hidData);
    phys[5] = POKEBOT_INPUT_HID_NEUTRAL;
    phys[6] = POKEBOT_INPUT_TOUCH_NEUTRAL;
    phys[7] = POKEBOT_INPUT_CIRCLE_NEUTRAL;
}

void PokebotInput_SetRawHid(u32 rawHid)
{
    u32 *phys = PA_FROM_VA_PTR(g_hidData);
    phys[5] = rawHid & POKEBOT_INPUT_HID_NEUTRAL;
    phys[6] = POKEBOT_INPUT_TOUCH_NEUTRAL;
    phys[7] = POKEBOT_INPUT_CIRCLE_NEUTRAL;
}

u32 PokebotInput_GetRawHid(void)
{
    u32 *phys = PA_FROM_VA_PTR(g_hidData);
    return phys[5];
}

bool PokebotInput_IsEnabled(void)
{
    return g_enabled;
}

bool PokebotInput_LegacyIsEnabled(void)
{
    return inputRedirectionEnabled;
}

Result PokebotInput_Enable(void)
{
    if(g_enabled)
        return 0;

    // Do not let the two HID owners compete. The experimental bridge is
    // deliberately proved with legacy Luma InputRedirection OFF.
    if(inputRedirectionEnabled)
        return -10;

    PokebotInput_ReleaseAll();

    svcKernelSetState(0x10000, 4);

    Handle hidProcess = 0;
    Result res = OpenProcessByName("hid", &hidProcess);
    if(R_SUCCEEDED(res))
        res = doHidPatch(hidProcess, true);

    svcKernelSetState(0x10000, 4);
    if(hidProcess != 0)
        svcCloseHandle(hidProcess);

    if(R_SUCCEEDED(res))
        g_enabled = true;

    return res;
}

Result PokebotInput_Disable(void)
{
    PokebotInput_ReleaseAll();

    if(!g_enabled)
        return 0;

    svcKernelSetState(0x10000, 4);

    Handle hidProcess = 0;
    Result res = OpenProcessByName("hid", &hidProcess);
    if(R_SUCCEEDED(res))
        res = doHidPatch(hidProcess, false);

    svcKernelSetState(0x10000, 4);
    if(hidProcess != 0)
        svcCloseHandle(hidProcess);

    if(R_SUCCEEDED(res))
        g_enabled = false;

    return res;
}
