/*
 * Pokebot3DS read-only Rosalina bridge.
 *
 * This module intentionally does not use GDB/debug attachment and exposes no
 * write primitive. It provides bounded, event-driven process reads over UDP.
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
#include "minisoc.h"
#include "pokebot_bridge.h"

#define REQ_MAGIC  0x5242524Fu /* ORBR */
#define RESP_MAGIC 0x5342524Fu /* ORBS */
#define WIRE_VERSION 1u

#define CMD_PING      1u
#define CMD_GAME_INFO 2u
#define CMD_QUERY     3u
#define CMD_READ      4u

#define STATUS_OK               0u
#define STATUS_BAD_MAGIC        1u
#define STATUS_BAD_VERSION      2u
#define STATUS_BAD_COMMAND      3u
#define STATUS_GAME_NOT_FOUND   4u
#define STATUS_OPEN_FAILED      5u
#define STATUS_QUERY_FAILED     6u
#define STATUS_NOT_READABLE     7u
#define STATUS_RANGE_INVALID    8u
#define STATUS_LENGTH_INVALID   9u
#define STATUS_MAP_FAILED      10u
#define STATUS_INTERNAL        11u

#define FLAG_READ_ONLY         (1u << 0)
#define FLAG_NO_GDB            (1u << 1)
#define FLAG_PAGE_MAPPED       (1u << 2)

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

static MyThread g_bridgeThread;
static u8 CTR_ALIGN(8) g_bridgeThreadStack[0x4000];
static void *g_scratch = NULL;
static u32 g_cachedPid = 0;
static u64 g_cachedTitleId = 0;
static char g_cachedName[8] = {0};

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
            struct pollfd pfd;
            pfd.fd = sock;
            pfd.events = POLLIN;
            pfd.revents = 0;

            int pollRes = socPoll(&pfd, 1, 50);
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

        socClose(sock);
        miniSocExit();

        if(!preTerminationRequested)
            svcSleepThread(250000000LL);
    }

    MyThread_Exit();
}
