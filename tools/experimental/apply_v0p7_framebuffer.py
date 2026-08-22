#!/usr/bin/env python3
from pathlib import Path


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected 1 match, found {count}")
    return text.replace(old, new, 1)


path = Path("sysmodules/rosalina/source/pokebot_bridge.c")
text = path.read_text(encoding="utf-8")

text = replace_once(text, '#include "csvc.h"\n#include "memory.h"', '#include "csvc.h"\n#include "draw.h"\n#include "memory.h"', "draw include")
text = replace_once(text, '#define CMD_INPUT_HID_LATCH   10u', '#define CMD_INPUT_HID_LATCH   10u\n#define CMD_FRAMEBUFFER_BEGIN 11u\n#define CMD_FRAMEBUFFER_READ  12u\n#define CMD_FRAMEBUFFER_END   13u', "commands")
text = replace_once(text, '#define STATUS_INPUT_PATCH_FAILED   15u', '#define STATUS_INPUT_PATCH_FAILED   15u\n#define STATUS_FB_BAD_CAPTURE       16u\n#define STATUS_FB_BUSY              17u\n#define STATUS_FB_ALLOC_FAILED      18u\n#define STATUS_FB_UNSUPPORTED       19u', "statuses")
text = replace_once(text, '#define SCRATCH_SIZE 0x2000u', '#define SCRATCH_SIZE 0x2000u\n#define FB_WIDTH 400u\n#define FB_HEIGHT 240u\n#define FB_BYTES (FB_WIDTH * FB_HEIGHT * 3u)\n#define FB_FORMAT_BGR8 1u\n#define FB_FLAG_BOTTOM_UP (1u << 0)\n#define FB_FLAG_TOP (1u << 1)\n#define FB_FLAG_LEFT (1u << 2)\n#define FB_FLAG_STABLE (1u << 3)\n#define FB_LIFETIME_MS 10000u', "constants")

payload = r'''
typedef struct __attribute__((packed))
{
    u32 captureId;
    u32 width;
    u32 height;
    u32 pixelFormat;
    u32 totalBytes;
    u32 maxChunkBytes;
    u32 flags;
    u32 fnv1a32;
} FramebufferInfoPayload;

'''
text = replace_once(text, 'typedef struct\n{\n    u32 sequenceId;\n    u32 terminalState;\n} RecentInputSequence;', payload + 'typedef struct\n{\n    u32 sequenceId;\n    u32 terminalState;\n} RecentInputSequence;', "payload")
text = replace_once(text, 'static u32 g_recentInputNext = 0;', 'static u32 g_recentInputNext = 0;\n\nstatic u8 *g_fb = NULL;\nstatic u32 g_fbCaptureId = 0;\nstatic u32 g_fbBeginRequestId = 0;\nstatic u32 g_fbFnv = 0;\nstatic u64 g_fbDeadlineMs = 0;\nstatic bool g_fbDrawLockHeld = false;', "state")

handlers = r'''
static u32 framebufferFnv1a32(const u8 *data, u32 length)
{
    u32 hash = 2166136261u;
    for(u32 i = 0; i < length; i++)
    {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static void releaseFramebufferSnapshot(void)
{
    if(g_fb != NULL)
        Draw_FreeFramebufferCache();
    g_fb = NULL;
    g_fbBeginRequestId = 0;
    g_fbFnv = 0;
    g_fbDeadlineMs = 0;
    if(g_fbDrawLockHeld)
    {
        g_fbDrawLockHeld = false;
        Draw_Unlock();
    }
}

static void serviceFramebufferSnapshot(void)
{
    if(g_fb != NULL && osGetTime() >= g_fbDeadlineMs)
        releaseFramebufferSnapshot();
}

static FramebufferInfoPayload framebufferInfo(void)
{
    FramebufferInfoPayload info;
    info.captureId = g_fbCaptureId;
    info.width = FB_WIDTH;
    info.height = FB_HEIGHT;
    info.pixelFormat = FB_FORMAT_BGR8;
    info.totalBytes = FB_BYTES;
    info.maxChunkBytes = POKEBOT_BRIDGE_MAX_READ;
    info.flags = FB_FLAG_BOTTOM_UP | FB_FLAG_TOP | FB_FLAG_LEFT | FB_FLAG_STABLE;
    info.fnv1a32 = g_fbFnv;
    return info;
}

static void handleFramebufferBegin(int sock, const struct sockaddr_in *peer,
                                   socklen_t peerLen, const BridgeRequest *req)
{
    serviceFramebufferSnapshot();
    if(req->requestId == 0 || req->argument != 0 || req->aux != 0)
    {
        sendResponse(sock, peer, peerLen, req, STATUS_FB_BAD_CAPTURE, -1, NULL, 0);
        return;
    }

    if(g_fb != NULL)
    {
        if(g_fbBeginRequestId == req->requestId)
        {
            FramebufferInfoPayload info = framebufferInfo();
            sendResponse(sock, peer, peerLen, req, STATUS_OK, 0, &info, sizeof(info));
        }
        else
            sendResponse(sock, peer, peerLen, req, STATUS_FB_BUSY, -1, NULL, 0);
        return;
    }

    Draw_Lock();
    g_fbDrawLockHeld = true;
    if(Draw_GetFramebufferCache() != NULL)
    {
        g_fbDrawLockHeld = false;
        Draw_Unlock();
        sendResponse(sock, peer, peerLen, req, STATUS_FB_BUSY, -1, NULL, 0);
        return;
    }

    u32 width = 0;
    bool is3d = false;
    Draw_GetCurrentScreenInfo(&width, &is3d, true);
    (void)is3d;
    if(width != FB_WIDTH)
    {
        g_fbDrawLockHeld = false;
        Draw_Unlock();
        sendResponse(sock, peer, peerLen, req, STATUS_FB_UNSUPPORTED, (Result)width, NULL, 0);
        return;
    }

    Result res = Draw_AllocateFramebufferCacheForScreenshot(FB_BYTES);
    if(R_FAILED(res) || Draw_GetFramebufferCacheSize() < FB_BYTES)
    {
        if(Draw_GetFramebufferCache() != NULL)
            Draw_FreeFramebufferCache();
        g_fbDrawLockHeld = false;
        Draw_Unlock();
        sendResponse(sock, peer, peerLen, req, STATUS_FB_ALLOC_FAILED, res, NULL, 0);
        return;
    }

    g_fb = (u8 *)Draw_GetFramebufferCache();
    svcFlushEntireDataCache();
    Draw_ConvertFrameBufferLines(g_fb, FB_WIDTH, 0, FB_HEIGHT, 1, true, true);

    g_fbCaptureId++;
    if(g_fbCaptureId == 0)
        g_fbCaptureId = 1;
    g_fbBeginRequestId = req->requestId;
    g_fbFnv = framebufferFnv1a32(g_fb, FB_BYTES);
    g_fbDeadlineMs = osGetTime() + FB_LIFETIME_MS;

    FramebufferInfoPayload info = framebufferInfo();
    sendResponse(sock, peer, peerLen, req, STATUS_OK, 0, &info, sizeof(info));
}

static void handleFramebufferRead(int sock, const struct sockaddr_in *peer,
                                  socklen_t peerLen, const BridgeRequest *req)
{
    serviceFramebufferSnapshot();
    if(g_fb == NULL || req->argument == 0 || req->argument != g_fbCaptureId)
    {
        sendResponse(sock, peer, peerLen, req, STATUS_FB_BAD_CAPTURE, -1, NULL, 0);
        return;
    }
    if(req->aux >= FB_BYTES)
    {
        sendResponse(sock, peer, peerLen, req, STATUS_FB_BAD_CAPTURE, -1, NULL, 0);
        return;
    }

    u32 remaining = FB_BYTES - req->aux;
    u32 length = remaining < POKEBOT_BRIDGE_MAX_READ ? remaining : POKEBOT_BRIDGE_MAX_READ;
    g_fbDeadlineMs = osGetTime() + FB_LIFETIME_MS;
    sendResponse(sock, peer, peerLen, req, STATUS_OK, 0, g_fb + req->aux, length);
}

static void handleFramebufferEnd(int sock, const struct sockaddr_in *peer,
                                 socklen_t peerLen, const BridgeRequest *req)
{
    serviceFramebufferSnapshot();
    if(req->argument == 0 || req->aux != 0)
    {
        sendResponse(sock, peer, peerLen, req, STATUS_FB_BAD_CAPTURE, -1, NULL, 0);
        return;
    }
    if(g_fb != NULL && req->argument != g_fbCaptureId)
    {
        sendResponse(sock, peer, peerLen, req, STATUS_FB_BAD_CAPTURE, -1, NULL, 0);
        return;
    }
    if(g_fb == NULL && req->argument != g_fbCaptureId)
    {
        sendResponse(sock, peer, peerLen, req, STATUS_FB_BAD_CAPTURE, -1, NULL, 0);
        return;
    }

    releaseFramebufferSnapshot();
    sendResponse(sock, peer, peerLen, req, STATUS_OK, 0, NULL, 0);
}

'''
text = replace_once(text, 'static void handlePacket(int sock, const u8 *data, size_t size,', handlers + 'static void handlePacket(int sock, const u8 *data, size_t size,', "handlers")
text = replace_once(text, '        case CMD_RELEASE_ALL:\n            handleReleaseAll(sock, peer, peerLen, &req);\n            break;\n        default:', '        case CMD_RELEASE_ALL:\n            handleReleaseAll(sock, peer, peerLen, &req);\n            break;\n        case CMD_FRAMEBUFFER_BEGIN:\n            handleFramebufferBegin(sock, peer, peerLen, &req);\n            break;\n        case CMD_FRAMEBUFFER_READ:\n            handleFramebufferRead(sock, peer, peerLen, &req);\n            break;\n        case CMD_FRAMEBUFFER_END:\n            handleFramebufferEnd(sock, peer, peerLen, &req);\n            break;\n        default:', "dispatch")
text = replace_once(text, '            serviceInputPulse();\n\n            struct pollfd pfd;', '            serviceInputPulse();\n            serviceFramebufferSnapshot();\n\n            struct pollfd pfd;', "snapshot service")
text = replace_once(text, '        PokebotInput_Disable();\n        socClose(sock);', '        PokebotInput_Disable();\n        releaseFramebufferSnapshot();\n        socClose(sock);', "reconnect cleanup")
text = replace_once(text, '    PokebotInput_ReleaseAll();\n    PokebotInput_Disable();\n    MyThread_Exit();', '    PokebotInput_ReleaseAll();\n    PokebotInput_Disable();\n    releaseFramebufferSnapshot();\n    MyThread_Exit();', "final cleanup")

for marker in ['#include "draw.h"', 'CMD_FRAMEBUFFER_BEGIN', 'CMD_FRAMEBUFFER_READ', 'CMD_FRAMEBUFFER_END', 'Draw_ConvertFrameBufferLines', 'FB_FLAG_STABLE', 'serviceFramebufferSnapshot()', 'case CMD_INPUT_TOUCH_PULSE:', 'case CMD_INPUT_HID_LATCH:']:
    if marker not in text:
        raise SystemExit(f"missing v0p7 marker: {marker}")

path.write_text(text, encoding="utf-8")
print("v0p7 framebuffer snapshot transform applied")
