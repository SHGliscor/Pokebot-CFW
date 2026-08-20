/*
 * Pokebot3DS standalone Luma sysmodule bridge v0p1.
 *
 * Experimental stock-Luma path. The process may be started before a game,
 * but it MUST NOT touch HID until CMD_INPUT_ARM is explicitly received and
 * a supported ORAS process is already running. CMD_INPUT_DISARM restores a
 * neutral remote state and removes the HID patch.
 *
 * RAM access is read-only. No GDB attachment and no game-memory writes.
 *
 * Portions of the HID redirection design are adapted from Luma3DS Rosalina
 * InputRedirection (GPLv3 or later).
 */

#include <3ds.h>
#include <3ds/allocator/mappable.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define BRIDGE_PORT 4952
#define REQ_MAGIC  0x5242524Fu /* ORBR */
#define RESP_MAGIC 0x5342524Fu /* ORBS */
#define WIRE_VERSION 1u

#define CMD_PING          1u
#define CMD_GAME_INFO     2u
#define CMD_QUERY         3u
#define CMD_READ          4u
#define CMD_INPUT_PING    5u
#define CMD_INPUT_PULSE   6u
#define CMD_INPUT_STATUS  7u
#define CMD_RELEASE_ALL   8u
#define CMD_INPUT_ARM     9u
#define CMD_INPUT_DISARM 10u

#define STATUS_OK                     0u
#define STATUS_BAD_MAGIC              1u
#define STATUS_BAD_VERSION            2u
#define STATUS_BAD_COMMAND            3u
#define STATUS_GAME_NOT_FOUND         4u
#define STATUS_OPEN_FAILED            5u
#define STATUS_QUERY_FAILED           6u
#define STATUS_NOT_READABLE           7u
#define STATUS_RANGE_INVALID          8u
#define STATUS_LENGTH_INVALID         9u
#define STATUS_MAP_FAILED            10u
#define STATUS_INTERNAL              11u
#define STATUS_INPUT_INVALID         12u
#define STATUS_INPUT_BUSY            13u
#define STATUS_INPUT_LEGACY_ACTIVE   14u
#define STATUS_INPUT_PATCH_FAILED    15u
#define STATUS_INPUT_NOT_ARMED       16u
#define STATUS_GAME_NOT_READY        17u

#define FLAG_READ_ONLY   (1u << 0)
#define FLAG_NO_GDB      (1u << 1)
#define FLAG_PAGE_MAPPED (1u << 2)

#define INPUT_CAP_HID_PULSE       (1u << 0)
#define INPUT_CAP_STATUS          (1u << 1)
#define INPUT_CAP_SEQUENCE_DEDUPE (1u << 2)
#define INPUT_CAP_RELEASE_ALL     (1u << 3)
#define INPUT_CAP_HID_ONLY_NO_IR  (1u << 4)
#define INPUT_CAP_LEGACY_OFF      (1u << 5)
#define INPUT_CAP_EXPLICIT_ARM    (1u << 6)
#define INPUT_CAP_DIRECT_HID      (1u << 7)

#define INPUT_RUNTIME_HID_ENABLED  (1u << 0)
#define INPUT_RUNTIME_PULSE_ACTIVE (1u << 2)
#define INPUT_RUNTIME_ARMED        (1u << 3)
#define INPUT_RUNTIME_GAME_SEEN    (1u << 4)

#define INPUT_STATE_IDLE              0u
#define INPUT_STATE_ACCEPTED          1u
#define INPUT_STATE_IN_PROGRESS       2u
#define INPUT_STATE_COMPLETED         3u
#define INPUT_STATE_ALREADY_COMPLETED 4u
#define INPUT_STATE_ABORTED           5u
#define INPUT_STATE_NOT_FOUND         6u

#define INPUT_HID_NEUTRAL    0x00000FFFu
#define INPUT_TOUCH_NEUTRAL  0x02000000u
#define INPUT_CIRCLE_NEUTRAL 0x007FF7FFu
#define INPUT_MAX_HOLD_MS    2000u
#define INPUT_MAX_SETTLE_MS  2000u
#define INPUT_MIN_HOLD_MS      20u
#define INPUT_RECENT_CACHE      16u

#define OR_TITLE_ID 0x000400000011C400ULL
#define AS_TITLE_ID 0x000400000011C500ULL
#define MAX_READ 512u
#define SCRATCH_SIZE 0x2000u
#define SOC_ALIGN 0x1000u
#define SOC_BUFFER_SIZE 0x100000u

extern u32 __ctru_heap;
extern u32 __ctru_heap_size;
extern u32 __ctru_linear_heap;
extern u32 __ctru_linear_heap_size;
extern char *fake_heap_start;
extern char *fake_heap_end;

Result svcConvertVAToPA(const void *va, bool writeCheck);
Result svcMapProcessMemoryEx(Handle dstProcessHandle, u32 destAddress,
                             Handle srcProcessHandle, u32 srcAddress,
                             u32 size, u32 flags);
Result svcUnmapProcessMemoryEx(Handle process, u32 destAddress, u32 size);
void hidCodePatchFunc(void);

#define PA_PTR(addr) ((void *)((u32)(addr) | (1u << 31)))
#define PA_FROM_VA_PTR(addr) PA_PTR(svcConvertVAToPA((const void *)(addr), false))

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

static void *g_scratch = NULL;
static void *g_socBuffer = NULL;
static u32 g_cachedPid = 0;
static u64 g_cachedTitleId = 0;
static char g_cachedName[8] = {0};
static bool g_gameSeen = false;

static bool g_inputArmed = false;
static bool g_hidPatched = false;
static bool g_inputActive = false;
static bool g_inputSettling = false;
static u32 g_inputSequenceId = 0;
static u32 g_inputRawHid = INPUT_HID_NEUTRAL;
static u32 g_inputSettleMs = 0;
static u64 g_inputDeadlineMs = 0;
static u64 g_armedTitleId = 0;
static u32 g_armedPid = 0;
static RecentInputSequence g_recentInput[INPUT_RECENT_CACHE] = {{0, 0}};
static u32 g_recentInputNext = 0;

/* Same 8-word shared layout used by Rosalina InputRedirection. */
static u32 g_hidData[] = {
    INPUT_HID_NEUTRAL, INPUT_TOUCH_NEUTRAL, INPUT_CIRCLE_NEUTRAL,
    0u, 0u,
    INPUT_HID_NEUTRAL, INPUT_TOUCH_NEUTRAL, INPUT_CIRCLE_NEUTRAL
};
static volatile u32 *g_remoteHidPhys = NULL;

static u8 *memsearchLocal(u8 *startPos, const void *pattern, u32 size, u32 patternSize)
{
    if(patternSize == 0 || size < patternSize)
        return NULL;

    const u8 *patternc = (const u8 *)pattern;
    u32 table[256];
    for(u32 i = 0; i < 256; i++)
        table[i] = patternSize;
    for(u32 i = 0; i < patternSize - 1; i++)
        table[patternc[i]] = patternSize - i - 1;

    u32 j = 0;
    while(j <= size - patternSize)
    {
        u8 c = startPos[j + patternSize - 1];
        if(patternc[patternSize - 1] == c &&
           memcmp(pattern, startPos + j, patternSize - 1) == 0)
            return startPos + j;
        j += table[c];
    }
    return NULL;
}

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

    Handle process = 0;
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
        Handle process = 0;
        res = svcOpenProcess(&process, pidList[i]);
        if(R_FAILED(res))
            continue;

        u64 titleId = 0;
        if(R_SUCCEEDED(svcGetProcessInfo((s64 *)&titleId, process, 0x10001)) &&
           isSupportedGame(titleId))
        {
            char name[8] = {0};
            (void)svcGetProcessInfo((s64 *)name, process, 0x10000);
            g_cachedPid = pidList[i];
            g_cachedTitleId = titleId;
            memcpy(g_cachedName, name, sizeof(g_cachedName));
            g_gameSeen = true;
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

static Result openProcessByName(const char *name, Handle *out)
{
    u32 pidList[0x40];
    s32 processCount = 0;
    Result res = svcGetProcessList(&processCount, pidList, 0x40);
    if(R_FAILED(res))
        return res;

    for(s32 i = 0; i < processCount; i++)
    {
        Handle process = 0;
        if(R_FAILED(svcOpenProcess(&process, pidList[i])))
            continue;

        char procName[8] = {0};
        if(R_SUCCEEDED(svcGetProcessInfo((s64 *)procName, process, 0x10000)) &&
           strncmp(procName, name, 8) == 0)
        {
            *out = process;
            return 0;
        }
        svcCloseHandle(process);
    }
    return -1;
}

static Result doUndoHidPatch(Handle processHandle, bool doPatch)
{
    static const u32 hidOrigRegisterAndValue[] = {0x1EC46000, 0x4001};
    static const u32 hidOrigCode[] = {
        0xE92D4070, 0xE1A05001, 0xEE1D4F70, 0xE3A01801, 0xE5A41080
    };
    static bool prepared = false;
    static u32 *hidRegPatchOffsets[2] = {NULL, NULL};
    static u32 *hidPatchJumpLoc = NULL;

    s64 textSize = 0, roSize = 0, dataSize = 0, startAddress = 0;
    (void)svcGetProcessInfo(&textSize, processHandle, 0x10002);
    (void)svcGetProcessInfo(&roSize, processHandle, 0x10003);
    (void)svcGetProcessInfo(&dataSize, processHandle, 0x10004);
    (void)svcGetProcessInfo(&startAddress, processHandle, 0x10005);

    const u32 totalSize = (u32)(textSize + roSize + dataSize);
    if(totalSize == 0)
        return -2;

    Result res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, 0x00100000,
                                       processHandle, (u32)startAddress,
                                       totalSize, 0);
    if(R_FAILED(res))
        return res;

    if(!prepared)
    {
        u32 *off = (u32 *)memsearchLocal((u8 *)0x00100000,
                                         hidOrigRegisterAndValue,
                                         totalSize,
                                         sizeof(hidOrigRegisterAndValue));
        if(off == NULL)
        {
            svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, 0x00100000, totalSize);
            return -3;
        }

        const u32 consumed = (u32)((u8 *)off + sizeof(hidOrigRegisterAndValue) - (u8 *)0x00100000);
        u32 *off2 = (u32 *)memsearchLocal((u8 *)off + sizeof(hidOrigRegisterAndValue),
                                          hidOrigRegisterAndValue,
                                          totalSize - consumed,
                                          sizeof(hidOrigRegisterAndValue));
        if(off2 == NULL)
        {
            svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, 0x00100000, totalSize);
            return -4;
        }

        u32 *off3 = (u32 *)memsearchLocal((u8 *)0x00100000,
                                          hidOrigCode,
                                          totalSize,
                                          sizeof(hidOrigCode));
        if(off3 == NULL)
        {
            svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, 0x00100000, totalSize);
            return -5;
        }

        hidRegPatchOffsets[0] = (u32 *)PA_FROM_VA_PTR(off);
        hidRegPatchOffsets[1] = (u32 *)PA_FROM_VA_PTR(off2);
        hidPatchJumpLoc = (u32 *)PA_FROM_VA_PTR(off3);
        prepared = true;
    }

    if(doPatch)
    {
        u32 hidDataPhys = (u32)PA_FROM_VA_PTR(g_hidData);
        u32 hidCodePhys = (u32)PA_FROM_VA_PTR(&hidCodePatchFunc);
        u32 hidHook[] = {
            0xE59F3004,
            0xE59FC004,
            0xE12FFF1C,
            hidDataPhys,
            hidCodePhys,
        };

        g_remoteHidPhys = ((volatile u32 *)PA_FROM_VA_PTR(g_hidData)) + 5;
        g_remoteHidPhys[0] = INPUT_HID_NEUTRAL;
        g_remoteHidPhys[1] = INPUT_TOUCH_NEUTRAL;
        g_remoteHidPhys[2] = INPUT_CIRCLE_NEUTRAL;

        *hidRegPatchOffsets[0] = hidDataPhys;
        *hidRegPatchOffsets[1] = hidDataPhys;
        memcpy(hidPatchJumpLoc, hidHook, sizeof(hidHook));
    }
    else
    {
        if(g_remoteHidPhys != NULL)
        {
            g_remoteHidPhys[0] = INPUT_HID_NEUTRAL;
            g_remoteHidPhys[1] = INPUT_TOUCH_NEUTRAL;
            g_remoteHidPhys[2] = INPUT_CIRCLE_NEUTRAL;
        }
        memcpy(hidRegPatchOffsets[0], hidOrigRegisterAndValue, sizeof(hidOrigRegisterAndValue));
        memcpy(hidRegPatchOffsets[1], hidOrigRegisterAndValue, sizeof(hidOrigRegisterAndValue));
        memcpy(hidPatchJumpLoc, hidOrigCode, sizeof(hidOrigCode));
        g_remoteHidPhys = NULL;
    }

    svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, 0x00100000, totalSize);
    return 0;
}

static Result setHidPatched(bool enable)
{
    if(enable == g_hidPatched)
        return 0;

    /* Match Rosalina's patch safety barrier around HID code modification. */
    (void)svcKernelSetState(0x10000, 4);

    Handle hidProcess = 0;
    Result res = openProcessByName("hid", &hidProcess);
    if(R_SUCCEEDED(res))
    {
        res = doUndoHidPatch(hidProcess, enable);
        svcCloseHandle(hidProcess);
    }

    (void)svcKernelSetState(0x10000, 4);

    if(R_SUCCEEDED(res))
        g_hidPatched = enable;
    return res;
}

static void writeRemoteHid(u32 rawHid)
{
    g_inputRawHid = rawHid & INPUT_HID_NEUTRAL;
    if(!g_hidPatched || g_remoteHidPhys == NULL)
        return;

    g_remoteHidPhys[0] = g_inputRawHid;
    g_remoteHidPhys[1] = INPUT_TOUCH_NEUTRAL;
    g_remoteHidPhys[2] = INPUT_CIRCLE_NEUTRAL;
}

static void releaseAll(void)
{
    writeRemoteHid(INPUT_HID_NEUTRAL);
}

static RecentInputSequence *findRecentInput(u32 sequenceId)
{
    for(u32 i = 0; i < INPUT_RECENT_CACHE; i++)
        if(g_recentInput[i].sequenceId == sequenceId)
            return &g_recentInput[i];
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
    releaseAll();
    g_inputActive = false;
    g_inputSettling = false;
    g_inputSettleMs = 0;
    g_inputDeadlineMs = 0;
    g_inputSequenceId = 0;
    rememberTerminalInput(sequenceId, terminalState);
}

static void serviceInputPulse(void)
{
    if(!g_inputActive)
        return;

    const u64 now = osGetTime();
    if(now < g_inputDeadlineMs)
        return;

    if(!g_inputSettling)
    {
        releaseAll();
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
    return now >= g_inputDeadlineMs ? 0u : (u32)(g_inputDeadlineMs - now);
}

static u32 inputRuntimeFlags(void)
{
    u32 flags = 0;
    if(g_hidPatched)
        flags |= INPUT_RUNTIME_HID_ENABLED;
    if(g_inputActive)
        flags |= INPUT_RUNTIME_PULSE_ACTIVE;
    if(g_inputArmed)
        flags |= INPUT_RUNTIME_ARMED;
    if(g_gameSeen)
        flags |= INPUT_RUNTIME_GAME_SEEN;
    return flags;
}

static InputStatusPayload makeInputStatus(u32 sequenceId, u32 state)
{
    InputStatusPayload payload;
    payload.sequenceId = sequenceId;
    payload.state = state;
    payload.rawHid = g_inputRawHid;
    payload.remainingMs = inputRemainingMs();
    payload.runtimeFlags = inputRuntimeFlags();
    return payload;
}

static int sendResponse(int sock, const struct sockaddr_in *peer, socklen_t peerLen,
                        const BridgeRequest *req, u16 status, Result result,
                        const void *payload, u32 payloadLen)
{
    u8 packet[sizeof(BridgeResponse) + MAX_READ];
    if(payloadLen > MAX_READ)
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

    return sendto(sock, packet, sizeof(response) + payloadLen, 0,
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
    Handle process = 0;
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
    Handle process = 0;
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
    if(length == 0 || length > MAX_READ)
    {
        sendResponse(sock, peer, peerLen, req, STATUS_LENGTH_INVALID, -1, NULL, 0);
        return;
    }

    const u64 requestEnd = (u64)address + (u64)length;
    if(requestEnd > 0xFFFFFFFFULL)
    {
        sendResponse(sock, peer, peerLen, req, STATUS_RANGE_INVALID, -1, NULL, 0);
        return;
    }

    Handle process = 0;
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

    const u64 regionEnd = (u64)mem.base_addr + (u64)mem.size;
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

    u8 payload[MAX_READ];
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
                              INPUT_CAP_HID_ONLY_NO_IR | INPUT_CAP_LEGACY_OFF |
                              INPUT_CAP_EXPLICIT_ARM | INPUT_CAP_DIRECT_HID;
    payload.runtimeFlags = inputRuntimeFlags();
    payload.neutralHid = INPUT_HID_NEUTRAL;
    payload.maxHoldMs = INPUT_MAX_HOLD_MS;
    payload.maxSettleMs = INPUT_MAX_SETTLE_MS;
    sendResponse(sock, peer, peerLen, req, STATUS_OK, 0, &payload, sizeof(payload));
}

static void handleInputArm(int sock, const struct sockaddr_in *peer, socklen_t peerLen,
                           const BridgeRequest *req)
{
    serviceInputPulse();
    if(g_inputArmed && g_hidPatched)
    {
        sendInputStatus(sock, peer, peerLen, req, STATUS_OK, 0, 0, INPUT_STATE_IDLE);
        return;
    }

    Handle process = 0;
    Result res = openGame(&process);
    if(R_FAILED(res))
    {
        sendInputStatus(sock, peer, peerLen, req, STATUS_GAME_NOT_READY, res,
                        0, INPUT_STATE_ABORTED);
        return;
    }
    svcCloseHandle(process);

    /* Hard safety rule: HID is first touched only after this game proof. */
    releaseAll();
    res = setHidPatched(true);
    if(R_FAILED(res))
    {
        sendInputStatus(sock, peer, peerLen, req, STATUS_INPUT_PATCH_FAILED, res,
                        0, INPUT_STATE_ABORTED);
        return;
    }

    g_inputArmed = true;
    g_armedTitleId = g_cachedTitleId;
    g_armedPid = g_cachedPid;
    releaseAll();
    sendInputStatus(sock, peer, peerLen, req, STATUS_OK, 0, 0, INPUT_STATE_IDLE);
}

static void handleInputDisarm(int sock, const struct sockaddr_in *peer, socklen_t peerLen,
                              const BridgeRequest *req)
{
    serviceInputPulse();
    if(g_inputActive)
        finishInputPulse(INPUT_STATE_ABORTED);
    else
        releaseAll();

    Result res = setHidPatched(false);
    if(R_FAILED(res))
    {
        sendInputStatus(sock, peer, peerLen, req, STATUS_INPUT_PATCH_FAILED, res,
                        0, INPUT_STATE_ABORTED);
        return;
    }

    g_inputArmed = false;
    g_armedTitleId = 0;
    g_armedPid = 0;
    g_inputRawHid = INPUT_HID_NEUTRAL;
    sendInputStatus(sock, peer, peerLen, req, STATUS_OK, 0, 0, INPUT_STATE_IDLE);
}

static void handleInputPulse(int sock, const struct sockaddr_in *peer, socklen_t peerLen,
                             const BridgeRequest *req)
{
    serviceInputPulse();

    if(!g_inputArmed || !g_hidPatched)
    {
        sendInputStatus(sock, peer, peerLen, req, STATUS_INPUT_NOT_ARMED, -1,
                        req->requestId, INPUT_STATE_ABORTED);
        return;
    }

    const u32 rawHid = req->argument;
    const u32 holdMs = req->aux & 0xFFFFu;
    const u32 settleMs = (req->aux >> 16) & 0xFFFFu;

    if(req->requestId == 0 ||
       (rawHid & ~INPUT_HID_NEUTRAL) != 0 ||
       rawHid == INPUT_HID_NEUTRAL ||
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

    writeRemoteHid(rawHid);
    g_inputActive = true;
    g_inputSettling = false;
    g_inputSequenceId = req->requestId;
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
        releaseAll();
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
            static const char pong[] = "POKEBOT3DS_STANDALONE_SYSMODULE_V0P1";
            sendResponse(sock, peer, peerLen, &req, STATUS_OK, 0,
                         pong, sizeof(pong) - 1);
            break;
        }
        case CMD_GAME_INFO: handleGameInfo(sock, peer, peerLen, &req); break;
        case CMD_QUERY: handleQuery(sock, peer, peerLen, &req); break;
        case CMD_READ: handleRead(sock, peer, peerLen, &req); break;
        case CMD_INPUT_PING: handleInputPing(sock, peer, peerLen, &req); break;
        case CMD_INPUT_PULSE: handleInputPulse(sock, peer, peerLen, &req); break;
        case CMD_INPUT_STATUS: handleInputStatus(sock, peer, peerLen, &req); break;
        case CMD_RELEASE_ALL: handleReleaseAll(sock, peer, peerLen, &req); break;
        case CMD_INPUT_ARM: handleInputArm(sock, peer, peerLen, &req); break;
        case CMD_INPUT_DISARM: handleInputDisarm(sock, peer, peerLen, &req); break;
        default:
            sendResponse(sock, peer, peerLen, &req, STATUS_BAD_COMMAND, -1, NULL, 0);
            break;
    }
}

void __system_allocateHeaps(void)
{
    u32 tmp = 0;
    __ctru_heap_size = 0x00400000;
    __ctru_heap = 0x08000000;
    svcControlMemory(&tmp, __ctru_heap, 0, __ctru_heap_size,
                     (MemOp)(MEMOP_ALLOC | MEMOP_REGION_BASE), MEMPERM_READWRITE);

    __ctru_linear_heap_size = 0x00100000;
    __ctru_linear_heap = 0x10000000;
    svcControlMemory(&tmp, __ctru_linear_heap, 0, __ctru_linear_heap_size,
                     (MemOp)(MEMOP_ALLOC_LINEAR | MEMOP_REGION_BASE), MEMPERM_READWRITE);

    fake_heap_start = (char *)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}

void __appInit(void)
{
    srvInit();
}

void __appExit(void)
{
    srvExit();
}

int main(void)
{
    /* Reserve a VA range only for temporary cross-process mappings. */
    mappableInit(0x11000000, 0x14000000);
    g_scratch = mappableAlloc(SCRATCH_SIZE);
    if(g_scratch == NULL)
        return 1;

    g_socBuffer = memalign(SOC_ALIGN, SOC_BUFFER_SIZE);
    if(g_socBuffer == NULL)
        return 2;

    Result res = socInit(g_socBuffer, SOC_BUFFER_SIZE);
    if(R_FAILED(res))
        return 3;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(sock < 0)
    {
        socExit();
        return 4;
    }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(BRIDGE_PORT);
    local.sin_addr.s_addr = INADDR_ANY;

    if(bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0)
    {
        closesocket(sock);
        socExit();
        return 5;
    }

    /* Critical invariant: no HID patch at module startup. */
    g_inputArmed = false;
    g_hidPatched = false;
    g_inputRawHid = INPUT_HID_NEUTRAL;

    for(;;)
    {
        serviceInputPulse();

        struct pollfd pfd;
        pfd.fd = sock;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pollRes = poll(&pfd, 1, 10);
        if(pollRes > 0 && (pfd.revents & POLLIN))
        {
            u8 packet[128];
            struct sockaddr_in peer;
            socklen_t peerLen = sizeof(peer);
            int got = recvfrom(sock, packet, sizeof(packet), 0,
                               (struct sockaddr *)&peer, &peerLen);
            if(got > 0)
                handlePacket(sock, packet, (size_t)got, &peer, peerLen);
        }
        else if(pollRes < 0)
        {
            svcSleepThread(100000000LL);
        }
    }

    return 0;
}
