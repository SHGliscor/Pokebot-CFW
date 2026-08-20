/*
 * Pokebot3DS acknowledged controller backend - v0p3.
 *
 * The v0p1/v0p2 private HID-only hook reproduced an existing failure mode on
 * hardware: once Wi-Fi/bridge controller ownership became active, normal
 * application button input could disappear while Rosalina itself still saw
 * keys. v0p3 therefore removes the duplicate HID patch entirely.
 *
 * Instead, the acknowledged UDP/4952 bridge temporarily OWNS the already
 * proven Nexus/Luma InputRedirection implementation and injects the exact
 * 12-byte HID/touch/circle packet format into the local UDP/4950 listener.
 * This gives us one HID implementation, one patch lifecycle and the same
 * physical-key passthrough behavior as the production controller path.
 *
 * Manual/external InputRedirection and Pokebot ownership are mutually
 * exclusive. The public 4950 path itself is not removed or changed.
 */

#include <3ds.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

#include "input_redirection.h"
#include "minisoc.h"
#include "pokebot_input.h"

#define INPUT_REDIRECTION_PORT 4950
#define INPUT_REDIRECTION_START_TIMEOUT_NS (10LL * 1000LL * 1000LL * 1000LL)
#define INPUT_REDIRECTION_STOP_TIMEOUT_NS  (5LL * 1000LL * 1000LL * 1000LL)

static bool g_enabled = false;
static bool g_ownedInputRedirection = false;
static int g_injectSocket = -1;
static u32 g_lastRawHid = POKEBOT_INPUT_HID_NEUTRAL;

static void closeInjectSocket(void)
{
    if(g_injectSocket >= 0)
    {
        socClose(g_injectSocket);
        g_injectSocket = -1;
    }
}

static Result openInjectSocket(void)
{
    if(g_injectSocket >= 0)
        return 0;

    int sock = socSocket(AF_INET, SOCK_DGRAM, 0);
    if(sock < 0)
        return (Result)sock;

    g_injectSocket = sock;
    return 0;
}

static Result sendInputPacket(u32 rawHid)
{
    if(!g_ownedInputRedirection || !inputRedirectionEnabled)
        return -20;

    Result res = openInjectSocket();
    if(R_FAILED(res))
        return res;

    const u32 packet[3] = {
        rawHid & POKEBOT_INPUT_HID_NEUTRAL,
        POKEBOT_INPUT_TOUCH_NEUTRAL,
        POKEBOT_INPUT_CIRCLE_NEUTRAL,
    };

    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons(INPUT_REDIRECTION_PORT);
    peer.sin_addr.s_addr = socGethostid();

    int sent = socSendto(g_injectSocket, packet, sizeof(packet), 0,
                         (const struct sockaddr *)&peer, sizeof(peer));
    if(sent != (int)sizeof(packet))
        return sent < 0 ? (Result)sent : -21;

    g_lastRawHid = packet[0];
    return 0;
}

bool PokebotInput_IsEnabled(void)
{
    return g_enabled && g_ownedInputRedirection && inputRedirectionEnabled;
}

bool PokebotInput_LegacyIsEnabled(void)
{
    // Report only an EXTERNALLY-owned/manual legacy session as a conflict.
    // The internally-owned session is the v0p3 backend itself.
    return inputRedirectionEnabled && !g_ownedInputRedirection;
}

Result PokebotInput_Enable(void)
{
    if(PokebotInput_IsEnabled())
        return 0;

    // Never steal a manually-enabled 4950 controller session.
    if(inputRedirectionEnabled && !g_ownedInputRedirection)
        return -10;

    // Recover from an interrupted partial ownership state conservatively.
    if(g_ownedInputRedirection && !inputRedirectionEnabled)
    {
        closeInjectSocket();
        g_ownedInputRedirection = false;
        g_enabled = false;
        g_lastRawHid = POKEBOT_INPUT_HID_NEUTRAL;
    }

    Result res = InputRedirection_DoOrUndoPatches();
    if(R_FAILED(res))
        return res;

    res = svcCreateEvent(&inputRedirectionThreadStartedEvent, RESET_STICKY);
    if(R_FAILED(res))
    {
        InputRedirection_DoOrUndoPatches();
        return res;
    }

    inputRedirectionStartResult = 0;
    inputRedirectionCreateThread();

    res = svcWaitSynchronization(inputRedirectionThreadStartedEvent,
                                 INPUT_REDIRECTION_START_TIMEOUT_NS);
    if(R_SUCCEEDED(res))
        res = (Result)inputRedirectionStartResult;

    inputRedirectionStartResult = 0;

    if(R_FAILED(res))
    {
        svcCloseHandle(inputRedirectionThreadStartedEvent);
        InputRedirection_DoOrUndoPatches();
        inputRedirectionEnabled = false;
        return res;
    }

    g_ownedInputRedirection = true;
    g_enabled = true;
    g_lastRawHid = POKEBOT_INPUT_HID_NEUTRAL;

    res = openInjectSocket();
    if(R_FAILED(res))
    {
        PokebotInput_Disable();
        return res;
    }

    // Establish a known neutral remote state before accepting the first pulse.
    res = sendInputPacket(POKEBOT_INPUT_HID_NEUTRAL);
    if(R_FAILED(res))
    {
        PokebotInput_Disable();
        return res;
    }

    return 0;
}

Result PokebotInput_Disable(void)
{
    if(!g_ownedInputRedirection)
    {
        closeInjectSocket();
        g_enabled = false;
        g_lastRawHid = POKEBOT_INPUT_HID_NEUTRAL;
        return 0;
    }

    // Best-effort neutral before tearing down the proven InputRedirection
    // patch/thread. A failed neutral must not prevent ownership teardown.
    (void)sendInputPacket(POKEBOT_INPUT_HID_NEUTRAL);
    closeInjectSocket();

    Result res = InputRedirection_Disable(INPUT_REDIRECTION_STOP_TIMEOUT_NS);

    g_ownedInputRedirection = false;
    g_enabled = false;
    g_lastRawHid = POKEBOT_INPUT_HID_NEUTRAL;

    return res;
}

void PokebotInput_SetRawHid(u32 rawHid)
{
    if(!PokebotInput_IsEnabled())
        return;

    (void)sendInputPacket(rawHid);
}

u32 PokebotInput_GetRawHid(void)
{
    return g_lastRawHid;
}

void PokebotInput_ReleaseAll(void)
{
    if(!PokebotInput_IsEnabled())
    {
        g_lastRawHid = POKEBOT_INPUT_HID_NEUTRAL;
        return;
    }

    (void)sendInputPacket(POKEBOT_INPUT_HID_NEUTRAL);
}
