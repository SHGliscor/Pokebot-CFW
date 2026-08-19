/*
 * Pokebot3DS Rosalina bridge.
 *
 * RAM commands remain read-only and intentionally do not use GDB/debug
 * attachment or expose any game-memory write primitive. Controller commands
 * are an additive HID-only transport with acknowledgement, sequence dedupe,
 * bounded timed pulses, and emergency release.
 */

#include <3ds.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

#include "csvc.h"
#include "memory.h"
#include "menu.h"
#include "minisoc.h"
#include "pokebot_bridge.h"
#include "pokebot_input.h"

#define REQ_MAGIC  0x5242524Fu /* ORBR */
#define RESP_MAGIC 0x5342524Fu /* ORBS */
#define WIRE_VERSION 1u

#define CMD_PING         1u
#define CMD_GAME_INFO    2u
#define CMD_QUERY        3u
#define CMD_READ         4u
#define CMD_INPUT_PING   5u
#define CMD_INPUT_PULSE  6u
#define CMD_INPUT_STATUS 7u
#define CMD_RELEASE_ALL  8u

#define STATUS_OK                    0u
#define STATUS_BAD_MAGIC             1u
#define STATUS_BAD_VERSION           2u
#define STATUS_BAD_COMMAND           3u
#define STATUS_GAME_NOT_FOUND        4u
#define STATUS_OPEN_FAILED           5u
#define STATUS_QUERY_FAILED          6u
#define STATUS_NOT_READABLE          7u
#define STATUS_RANGE_INVALID         8u
#define STATUS_LENGTH_INVALID        9u
#define STATUS_MAP_FAILED           10u
#define STATUS_INTERNAL             11u
#define STATUS_INPUT_INVALID        12u
#define STATUS_INPUT_BUSY           13u
#define STATUS_INPUT_LEGACY_ACTIVE  14u
#define STATUS_INPUT_PATCH_FAILED   15u

#define FLAG_READ_ONLY         (1u << 0)
#define FLAG_NO_GDB            (1u << 1)
#define FLAG_PAGE_MAPPED       (1u << 2)

#define INPUT_CAP_HID_PULSE       (1u << 0)
#define INPUT_CAP_STATUS          (1u << 1)
#define INPUT_CAP_SEQUENCE_DEDUPE (1u << 2)
#define INPUT_CAP_RELEASE_ALL     (1u << 3)
#define INPUT_CAP_HID_ONLY_NO_IR  (1u << 4)
#define INPUT_CAP_LEGACY_OFF      (1u << 5)

#define INPUT_RUNTIME_HID_ENABLED   (1u << 0)
#define INPUT_RUNTIME_LEGACY_ACTIVE (1u << 1)
#define INPUT_RUNTIME_PULSE_ACTIVE  (1u << 2)

#define INPUT_STATE_IDLE              0u
#define INPUT_STATE_ACCEPTED          1u
#define INPUT_STATE_IN_PROGRESS       2u
#define INPUT_STATE_COMPLETED         3u
#define INPUT_STATE_ALREADY_COMPLETED 4u
#define INPUT_STATE_ABORTED           5u
#define INPUT_STATE_NOT_FOUND         6u

#define INPUT_MAX_HOLD_MS   2000u
#define INPUT_MAX_SETTLE_MS 2000u
#define INPUT_MIN_HOLD_MS     20u
#define INPUT_RECENT_CACHE     16u

#define OR_TITLE_ID 0x000400000011C400ULL
#define AS_TITLE_ID 0x000400000011C500ULL
#define SCRATCH_SIZE 0x2000u

extern bool preTerminationRequested;

typedef struct __attribute__((packed))
{
    u32 magic;
    u16 version;
    u16 command;
    u32 requestId;
    u32 argument;
    u32 aux;
} BridgeRequest;

typedef struct __attribute__((packed))
{
    u32 magic;
    u16 version;
    u16 status;
    u32 requestId;
    u32 argument;
    s32 result;
    u32 payloadLen;
} BridgeResponse;

typedef struct __attribute__((packed))
{
    u64 titleId;
    u32 pid;
    char name[8];
    u32 flags;
} GameInfoPayload;

typedef struct __attribute__((packed))
{
    u32 baseAddr;
    u32 size;
    u32 perm;
    u32 state;
    u32 pageFlags;
} QueryPayload;

typedef struct __attribute__((packed))
{
    u32 protocolVersion;
    u32 capabilityFlags;
    u32 runtimeFlags;
    u32 neutralHid;
    u32 maxHoldMs;
    u32 maxSettleMs;
} InputCapabilitiesPayload;

typedef struct __attribute__((packed))
{
    u32 sequenceId;
    u32 state;
    u32 rawHid;
    u32 remainingMs;
    u32 runtimeFlags;
} InputStatusPayload;

typedef struct
{
    u32 sequenceId;
    u32 terminalState;
} RecentInputSequence;

static MyThread g_bridgeThread;
static u8 CTR_ALIGN(8) g_bridgeThreadStack[0x4000];
static void *g_scratch = NULL;
static u32 g_cachedPid = 0;
static u64 g_cachedTitleId = 0;
static char g_cachedName[8] = {0};

static bool g_inputActive = false;
static bool g_inputSettling = false;
static u32 g_inputSequenceId = 0;
static u32 g_inputRawHid = POKEBOT_INPUT_HID_NEUTRAL;
static u32 g_inputSettleMs = 0;
static u64 g_inputDeadlineMs = 0;
static RecentInputSequence g_recentInput[INPUT_RECENT_CACHE] = {{0, 0}};
static u32 g_recentInputNext = 0;

static bool isSupportedGame(u64 titleId)
{
    return titleId == OR_TITLE_ID || titleId == AS_TITLE_ID;
}

static void clearCachedGame(void)
{
    g_cachedPid = 0;
    g_cachedTitleId = 0;
    memset(g_cachedName, 0, sizeof(g_cachedName));
}

static Result openCachedGame(Handle *out)
{
    if(g_cachedPid == 0)
        return -1;

    Handle process;
    Result res = svcOpenProcess(&process, g_cachedPid);
    if(R_FAILED(res))
    {
        clearCachedGame();
        return res;
    }

    u64 titleId = 0;
    res = svcGetProcessInfo((s64 *)&titleId, process, 0x10001);
    if(R_FAILED(res) || titleId != g_cachedTitleId || !isSupportedGame(titleId))
    {
        svcCloseHandle(process);
        clearCachedGame();
        return R_FAILED(res) ? res : -1;
    }

    *out = process;
    return 0;
}

static Result discoverGame(Handle *out)
{
    u32 pidList[0x40];
    s32 processCount = 0;
    Result res = svcGetProcessList(&processCount, pidList, 0x40);
    if(R_FAILED(res))
        return res;

    for(s32 i = 0; i < processCount; i++)
    {
        Handle process;
        res = svcOpenProcess(&process, pidList[i]);
        if(R_FAILED(res))
            continue;

        u64 titleId = 0;
        if(R_SUCCEEDED(svcGetProcessInfo((s64 *)&titleId, process, 0x10001)) && isSupportedGame(titleId))
        {
            char name[8] = {0};
            svcGetProcessInfo((s64 *)name, process, 0x10000);

            g_cachedPid = pidList[i];
            g_cachedTitleId = titleId;
            memcpy(g_cachedName, name, sizeof(g_cachedName));
            *out = process;
            return 0;
        }

        svcCloseHandle(process);
    }

    clearCachedGame();
    return -1;
}

static Result openGame(Handle *out)
{
    Result res = openCachedGame(out);
    if(R_SUCCEEDED(res))
        return res;
    return discoverGame(out);
}

static u32 inputRuntimeFlags(void)
{
    u32 flags = 0;
    if(PokebotInput_IsEnabled())
        flags |= INPUT_RUNTIME_HID_ENABLED;
    if(PokebotInput_LegacyIsEnabled())
        flags |= INPUT_RUNTIME_LEGACY_ACTIVE;
    if(g_inputActive)
        flags |= INPUT_RUNTIME_PULSE_ACTIVE;
    return flags;
}

static RecentInputSequence *findRecentInput(u32 sequenceId)
{
    for(u32 i = 0; i < INPUT_RECENT_CACHE; i++)
    {
        if(g_recentInput[i].sequenceId == sequenceId)
            return &g_recentInput[i];
    }
    return NULL;
}

static void rememberTerminalInput(u32 sequenceId, u32 terminalState)
{
    if(sequenceId == 0)
        return;

    RecentInputSequence *existing = findRecentInput(sequenceId);
    if(existing != NULL)
    {
        existing->terminalState = terminalState;
        return;
    }

    g_recentInput[g_recentInputNext].sequenceId = sequenceId;
    g_recentInput[g_recentInputNext].terminalState = terminalState;
    g_recentInputNext = (g_recentInputNext + 1u) % INPUT_RECENT_CACHE;
}

static void finishInputPulse(u32 terminalState)
{
    const u32 sequenceId = g_inputSequenceId;
    PokebotInput_ReleaseAll();
    g_inputActive = false;
    g_inputSettling = false;
    g_inputRawHid = POKEBOT_INPUT_HID_NEUTRAL;
    g_inputSettleMs = 0;
    g_inputDeadlineMs = 0;
    g_inputSequenceId = 0;
    rememberTerminalInput(sequenceId, terminalState);
}

static void serviceInputPulse(void)
{
    if(!g_inputActive)
        return;

    if(PokebotInput_LegacyIsEnabled())
    {
        finishInputPulse(INPUT_STATE_ABORTED);
        return;
    }

    const u64 now = osGetTime();
    if(now < g_inputDeadlineMs)
        return;

    if(!g_inputSettling)
    {
        PokebotInput_ReleaseAll();
        g_inputRawHid = POKEBOT_INPUT_HID_NEUTRAL;
        g_inputSettling = true;

        if(g_inputSettleMs == 0)
        {
            finishInputPulse(INPUT_STATE_COMPLETED);
            return;
        }

        g_inputDeadlineMs = now + g_inputSettleMs;
        return;
    }

    finishInputPulse(INPUT_STATE_COMPLETED);
}

static u32 inputRemainingMs(void)
{
    if(!g_inputActive)
        return 0;

    const u64 now = osGetTime();
    if(now >= g_inputDeadlineMs)
        return 0;

    return (u32)(g_inputDeadlineMs - now);
}

static InputStatusPayload makeInputStatus(u32 sequenceId, u32 state)
{
    InputStatusPayload payload;
    payload.sequenceId = sequenceId;
    payload.state = state;
    payload.rawHid = PokebotInput_GetRawHid();
    payload.remainingMs = inputRemainingMs();
    payload.runtimeFlags = inputRuntimeFlags();
    return payload;
}

static int sendResponse(int sock, const struct sockaddr_in *peer, socklen_t peerLen,
                        const BridgeRequest *req, u16 status, Result result,
                        const void *payload, u32 payloadLen)
{
    u8 packet[sizeof(BridgeResponse) + POKEBOT_BRIDGE_MAX_READ];
    if(payloadLen > POKEBOT_BRIDGE_MAX_READ)
        return -1;

    BridgeResponse response;
    response.magic = RESP_MAGIC;
    response.version = WIRE_VERSION;
    response.status = status;
    response.requestId = req->requestId;
    response.argument = req->argument;
    response.result = (s32)result;
    response.payloadLen = payloadLen;

    memcpy(packet, &response, sizeof(response));
    if(payloadLen != 0 && payload != NULL)
        memcpy(packet + sizeof(response), payload, payloadLen);

    return (int)socSendto(sock, packet, sizeof(response) + payloadLen, 0,
                          (const struct sockaddr *)peer, peerLen);
}

static void sendInputStatus(int sock, const struct sockaddr_in *peer, socklen_t peerLen,
                            const BridgeRequest *req, u16 bridgeStatus, Result result,
                            u32 sequenceId, u32 inputState)
{
    InputStatusPayload payload = makeInputStatus(sequenceId, inputState);
    sendResponse(sock, peer, peerLen, req, bridgeStatus, result, &payload, sizeof(payload));
}

static void handleGameInfo(int sock, const struct sockaddr_in *peer, socklen_t peerLen,
                           const BridgeRequest *req)
{
    Handle process;
    Result res = openGame(&process);
    if(R_FAILED(res))
    {
        sendResponse(sock, peer, peerLen, req, STATUS_GAME_NOT_FOUND, res, NULL, 0);
        return;
    }

    svcCloseHandle(process);

    GameInfoPayload info;
    info.titleId = g_cachedTitleId;
    info.pid = g_cachedPid;
    memcpy(info.name, g_cachedName, sizeof(info.name));
    info.flags = FLAG_READ_ONLY | FLAG_NO_GDB | FLAG_PAGE_MAPPED;

    sendResponse(sock, peer, peerLen, req, STATUS_OK, 0, &info, sizeof(info));
}

static void handleQuery(int sock, const struct sockaddr_in *peer, socklen_t peerLen,
                        const BridgeRequest *req)
{
    Handle process;
    Result res = openGame(&process);
    if(R_FAILED(res))
    {
        sendResponse(sock, peer, peerLen, req, STATUS_GAME_NOT_FOUND, res, NULL, 0);
        return;
    }

    MemInfo mem;
    PageInfo page;
    res = svcQueryProcessMemory(&mem, &page, process, req->argument);
    svcCloseHandle(process);

    if(R_FAILED(res))
    {
        sendResponse(sock, peer, peerLen, req, STATUS_QUERY_FAILED, res, NULL, 0);
        return;
    }

    QueryPayload payload = {mem.base_addr, mem.size, mem.perm, mem.state, page.flags};
    sendResponse(sock, peer, peerLen, req, STATUS_OK, res, &payload, sizeof(payload));
}

static void handleRead(int sock, const struct sockaddr_in *peer, socklen_t peerLen,
                       const BridgeRequest *req)
{
    const u32 address = req->argument;
    const u32 length = req->aux;

    if(length == 0 || length > POKEBOT_BRIDGE_MAX_READ)
    {
        sendResponse(sock, peer, peerLen, req, STATUS_LENGTH_INVALID, -1, NULL, 0);
        return;
    }

    u64 requestEnd = (u64)address + (u64)length;
    if(requestEnd > 0x100000000ULL)
    {
        sendResponse(sock, peer, peerLen, req, STATUS_RANGE_INVALID, -1, NULL, 0);
        return;
    }

    Handle process;
    Result res = openGame(&process);
    if(R_FAILED(res))
    {
        sendResponse(sock, peer, peerLen, req, STATUS_GAME_NOT_FOUND, res, NULL, 0);
        return;
    }

    MemInfo mem;
    PageInfo page;
    res = svcQueryProcessMemory(&mem, &page, process, address);
    if(R_FAILED(res))
    {
        svcCloseHandle(process);
        sendResponse(sock, peer, peerLen, req, STATUS_QUERY_FAILED, res, NULL, 0);
        return;
    }

    if((mem.perm & MEMPERM_READ) == 0)
    {
        svcCloseHandle(process);
        sendResponse(sock, peer, peerLen, req, STATUS_NOT_READABLE, 0, NULL, 0);
        return;
    }

    u64 regionEnd = (u64)mem.base_addr + (u64)mem.size;
    if(address < mem.base_addr || requestEnd > regionEnd)
    {
        svcCloseHandle(process);
        sendResponse(sock, peer, peerLen, req, STATUS_RANGE_INVALID, 0, NULL, 0);
        return;
    }

    const u32 sourceBase = address & ~0xFFFu;
    const u32 sourceEnd = ((u32)requestEnd + 0xFFFu) & ~0xFFFu;
    const u32 mapSize = sourceEnd - sourceBase;
    const u32 offset = address - sourceBase;

    if(mapSize == 0 || mapSize > SCRATCH_SIZE || g_scratch == NULL)
    {
        svcCloseHandle(process);
        sendResponse(sock, peer, peerLen, req, STATUS_INTERNAL, -1, NULL, 0);
        return;
    }

    res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, (u32)g_scratch,
                                process, sourceBase, mapSize, 0);
    if(R_FAILED(res))
    {
        svcCloseHandle(process);
        sendResponse(sock, peer, peerLen, req, STATUS_MAP_FAILED, res, NULL, 0);
        return;
    }

    u8 payload[POKEBOT_BRIDGE_MAX_READ];
    memcpy(payload, (const u8 *)g_scratch + offset, length);

    Result unmapRes = svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, (u32)g_scratch, mapSize);
    svcCloseHandle(process);

    if(R_FAILED(unmapRes))
    {
        sendResponse(sock, peer, peerLen, req, STATUS_INTERNAL, unmapRes, NULL, 0);
        return;
    }

    sendResponse(sock, peer, peerLen, req, STATUS_OK, 0, payload, length);
}

static void handleInputPing(int sock, const struct sockaddr_in *peer, socklen_t peerLen,
                            const BridgeRequest *req)
{
    serviceInputPulse();

    InputCapabilitiesPayload payload;
    payload.protocolVersion = 1u;
    payload.capabilityFlags = INPUT_CAP_HID_PULSE | INPUT_CAP_STATUS |
                              INPUT_CAP_SEQUENCE_DEDUPE | INPUT_CAP_RELEASE_ALL |
                              INPUT_CAP_HID_ONLY_NO_IR | INPUT_CAP_LEGACY_OFF;
    payload.runtimeFlags = inputRuntimeFlags();
    payload.neutralHid = POKEBOT_INPUT_HID_NEUTRAL;
    payload.maxHoldMs = INPUT_MAX_HOLD_MS;
    payload.maxSettleMs = INPUT_MAX_SETTLE_MS;

    sendResponse(sock, peer, peerLen, req, STATUS_OK, 0, &payload, sizeof(payload));
}

static void handleInputPulse(int sock, const struct sockaddr_in *peer, socklen_t peerLen,
                             const BridgeRequest *req)
{
    serviceInputPulse();

    const u32 rawHid = req->argument;
    const u32 holdMs = req->aux & 0xFFFFu;
    const u32 settleMs = (req->aux >> 16) & 0xFFFFu;

    if(req->requestId == 0 ||
       (rawHid & ~POKEBOT_INPUT_HID_NEUTRAL) != 0 ||
       rawHid == POKEBOT_INPUT_HID_NEUTRAL ||
       holdMs < INPUT_MIN_HOLD_MS || holdMs > INPUT_MAX_HOLD_MS ||
       settleMs > INPUT_MAX_SETTLE_MS)
    {
        sendInputStatus(sock, peer, peerLen, req, STATUS_INPUT_INVALID, -1,
                        req->requestId, INPUT_STATE_ABORTED);
        return;
    }

    if(g_inputActive && g_inputSequenceId == req->requestId)
    {
        sendInputStatus(sock, peer, peerLen, req, STATUS_OK, 0,
                        req->requestId, INPUT_STATE_IN_PROGRESS);
        return;
    }

    RecentInputSequence *recent = findRecentInput(req->requestId);
    if(recent != NULL)
    {
        const u32 state = recent->terminalState == INPUT_STATE_COMPLETED
                        ? INPUT_STATE_ALREADY_COMPLETED
                        : recent->terminalState;
        sendInputStatus(sock, peer, peerLen, req, STATUS_OK, 0,
                        req->requestId, state);
        return;
    }

    if(g_inputActive)
    {
        sendInputStatus(sock, peer, peerLen, req, STATUS_INPUT_BUSY, -1,
                        g_inputSequenceId, INPUT_STATE_IN_PROGRESS);
        return;
    }

    if(PokebotInput_LegacyIsEnabled())
    {
        sendInputStatus(sock, peer, peerLen, req, STATUS_INPUT_LEGACY_ACTIVE, -1,
                        req->requestId, INPUT_STATE_ABORTED);
        return;
    }

    Result res = PokebotInput_Enable();
    if(R_FAILED(res))
    {
        sendInputStatus(sock, peer, peerLen, req, STATUS_INPUT_PATCH_FAILED, res,
                        req->requestId, INPUT_STATE_ABORTED);
        return;
    }

    PokebotInput_SetRawHid(rawHid);
    g_inputActive = true;
    g_inputSettling = false;
    g_inputSequenceId = req->requestId;
    g_inputRawHid = rawHid;
    g_inputSettleMs = settleMs;
    g_inputDeadlineMs = osGetTime() + holdMs;

    sendInputStatus(sock, peer, peerLen, req, STATUS_OK, 0,
                    req->requestId, INPUT_STATE_ACCEPTED);
}

static void handleInputStatus(int sock, const struct sockaddr_in *peer, socklen_t peerLen,
                              const BridgeRequest *req)
{
    serviceInputPulse();

    const u32 targetSequence = req->argument;
    if(targetSequence == 0)
    {
        const u32 state = g_inputActive ? INPUT_STATE_IN_PROGRESS : INPUT_STATE_IDLE;
        const u32 sequence = g_inputActive ? g_inputSequenceId : 0;
        sendInputStatus(sock, peer, peerLen, req, STATUS_OK, 0, sequence, state);
        return;
    }

    if(g_inputActive && g_inputSequenceId == targetSequence)
    {
        sendInputStatus(sock, peer, peerLen, req, STATUS_OK, 0,
                        targetSequence, INPUT_STATE_IN_PROGRESS);
        return;
    }

    RecentInputSequence *recent = findRecentInput(targetSequence);
    if(recent != NULL)
    {
        sendInputStatus(sock, peer, peerLen, req, STATUS_OK, 0,
                        targetSequence, recent->terminalState);
        return;
    }

    sendInputStatus(sock, peer, peerLen, req, STATUS_OK, 0,
                    targetSequence, INPUT_STATE_NOT_FOUND);
}

static void handleReleaseAll(int sock, const struct sockaddr_in *peer, socklen_t peerLen,
                             const BridgeRequest *req)
{
    serviceInputPulse();

    if(g_inputActive)
        finishInputPulse(INPUT_STATE_ABORTED);
    else
        PokebotInput_ReleaseAll();

    sendInputStatus(sock, peer, peerLen, req, STATUS_OK, 0, 0, INPUT_STATE_IDLE);
}

static void handlePacket(int sock, const u8 *data, size_t size,
                         const struct sockaddr_in *peer, socklen_t peerLen)
{
    if(size < sizeof(BridgeRequest))
        return;

    BridgeRequest req;
    memcpy(&req, data, sizeof(req));

    if(req.magic != REQ_MAGIC)
    {
        sendResponse(sock, peer, peerLen, &req, STATUS_BAD_MAGIC, -1, NULL, 0);
        return;
    }

    if(req.version != WIRE_VERSION)
    {
        sendResponse(sock, peer, peerLen, &req, STATUS_BAD_VERSION, -1, NULL, 0);
        return;
    }

    switch(req.command)
    {
        case CMD_PING:
        {
            // Keep the original payload for compatibility with the proven PC
            // RAM client. Input features are discovered via CMD_INPUT_PING.
            static const char pong[] = "POKEBOT_CFW_BRIDGE_V0P1";
            sendResponse(sock, peer, peerLen, &req, STATUS_OK, 0, pong, sizeof(pong) - 1);
            break;
        }
        case CMD_GAME_INFO:
            handleGameInfo(sock, peer, peerLen, &req);
            break;
        case CMD_QUERY:
            handleQuery(sock, peer, peerLen, &req);
            break;
        case CMD_READ:
            handleRead(sock, peer, peerLen, &req);
            break;
        case CMD_INPUT_PING:
            handleInputPing(sock, peer, peerLen, &req);
            break;
        case CMD_INPUT_PULSE:
            handleInputPulse(sock, peer, peerLen, &req);
            break;
        case CMD_INPUT_STATUS:
            handleInputStatus(sock, peer, peerLen, &req);
            break;
        case CMD_RELEASE_ALL:
            handleReleaseAll(sock, peer, peerLen, &req);
            break;
        default:
            sendResponse(sock, peer, peerLen, &req, STATUS_BAD_COMMAND, -1, NULL, 0);
            break;
    }
}

MyThread *PokebotBridge_CreateThread(void)
{
    if(R_FAILED(MyThread_Create(&g_bridgeThread, PokebotBridge_ThreadMain,
                                g_bridgeThreadStack, sizeof(g_bridgeThreadStack),
                                0x20, CORE_SYSTEM)))
        svcBreak(USERBREAK_PANIC);
    return &g_bridgeThread;
}

void PokebotBridge_ThreadMain(void)
{
    PokebotInput_ReleaseAll();

    if(g_scratch == NULL)
        g_scratch = mappableAlloc(SCRATCH_SIZE);

    if(g_scratch == NULL)
    {
        MyThread_Exit();
        return;
    }

    while(!preTerminationRequested)
    {
        Result res = miniSocInit();
        if(R_FAILED(res))
        {
            svcSleepThread(1000000000LL);
            continue;
        }

        int sock = socSocket(AF_INET, SOCK_DGRAM, 0);
        if(sock < 0)
        {
            miniSocExit();
            svcSleepThread(1000000000LL);
            continue;
        }

        struct sockaddr_in local;
        memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        local.sin_port = htons(POKEBOT_BRIDGE_PORT);
        local.sin_addr.s_addr = socGethostid();

        if(socBind(sock, (struct sockaddr *)&local, sizeof(local)) != 0)
        {
            socClose(sock);
            miniSocExit();
            svcSleepThread(1000000000LL);
            continue;
        }

        while(!preTerminationRequested)
        {
            serviceInputPulse();

            struct pollfd pfd;
            pfd.fd = sock;
            pfd.events = POLLIN;
            pfd.revents = 0;

            // 10 ms wake interval bounds pulse-release timing without polling
            // game RAM. Network work remains event driven.
            int pollRes = socPoll(&pfd, 1, 10);
            if(pollRes > 0 && (pfd.revents & POLLIN))
            {
                u8 packet[128];
                struct sockaddr_in peer;
                socklen_t peerLen = sizeof(peer);
                int got = (int)socRecvfrom(sock, packet, sizeof(packet), 0,
                                           (struct sockaddr *)&peer, &peerLen);
                if(got > 0)
                    handlePacket(sock, packet, (size_t)got, &peer, peerLen);
            }
            else if(pollRes < -10000)
            {
                break;
            }
        }

        if(g_inputActive)
            finishInputPulse(INPUT_STATE_ABORTED);
        else
            PokebotInput_ReleaseAll();

        PokebotInput_Disable();
        socClose(sock);
        miniSocExit();

        if(!preTerminationRequested)
            svcSleepThread(250000000LL);
    }

    PokebotInput_ReleaseAll();
    PokebotInput_Disable();
    MyThread_Exit();
}
