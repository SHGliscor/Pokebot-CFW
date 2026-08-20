#!/usr/bin/env python3
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly 1 match, found {count}")
    return text.replace(old, new, 1)


# ---------------------------------------------------------------------------
# v0p6 additive latched-HID transform.
#
# This script runs AFTER apply_v0p5_touch.py in CI. It does not change the
# proven timed HID pulse (command 6), status (7), RELEASE_ALL (8), or native
# touch pulse (9) semantics.
#
# New command 10 latches one non-neutral HID state until RELEASE_ALL (8),
# bridge teardown, or legacy-input conflict. There is deliberately no timed
# neutral gap, which is required for a true Acro Bike B hold.
# ---------------------------------------------------------------------------

bridge_path = Path("sysmodules/rosalina/source/pokebot_bridge.c")
bridge = bridge_path.read_text(encoding="utf-8")

bridge = replace_once(
    bridge,
    "#define CMD_INPUT_TOUCH_PULSE 9u",
    "#define CMD_INPUT_TOUCH_PULSE 9u\n#define CMD_INPUT_HID_LATCH   10u",
    "latched HID command define",
)

bridge = replace_once(
    bridge,
    "#define INPUT_CAP_TOUCH_PULSE     (1u << 6)",
    "#define INPUT_CAP_TOUCH_PULSE     (1u << 6)\n"
    "#define INPUT_CAP_HID_LATCH       (1u << 7)",
    "latched HID capability define",
)

bridge = replace_once(
    bridge,
    "#define INPUT_RUNTIME_TOUCH_ACTIVE  (1u << 3)",
    "#define INPUT_RUNTIME_TOUCH_ACTIVE  (1u << 3)\n"
    "#define INPUT_RUNTIME_HID_LATCHED   (1u << 4)",
    "latched HID runtime define",
)

bridge = replace_once(
    bridge,
    "#define INPUT_KIND_TOUCH 2u",
    "#define INPUT_KIND_TOUCH     2u\n"
    "#define INPUT_KIND_HID_LATCH 3u",
    "latched HID input kind",
)

bridge = replace_once(
    bridge,
    "    if(g_inputActive && g_inputKind == INPUT_KIND_TOUCH)\n"
    "        flags |= INPUT_RUNTIME_TOUCH_ACTIVE;\n"
    "    return flags;",
    "    if(g_inputActive && g_inputKind == INPUT_KIND_TOUCH)\n"
    "        flags |= INPUT_RUNTIME_TOUCH_ACTIVE;\n"
    "    if(g_inputActive && g_inputKind == INPUT_KIND_HID_LATCH)\n"
    "        flags |= INPUT_RUNTIME_HID_LATCHED;\n"
    "    return flags;",
    "latched HID runtime flag",
)

# A latched HID input must never enter the timed pulse deadline/release path.
# Legacy/manual InputRedirection conflict is still fail-closed and releases it.
bridge = replace_once(
    bridge,
    "    if(PokebotInput_LegacyIsEnabled())\n"
    "    {\n"
    "        finishInputPulse(INPUT_STATE_ABORTED);\n"
    "        return;\n"
    "    }\n\n"
    "    const u64 now = osGetTime();",
    "    if(PokebotInput_LegacyIsEnabled())\n"
    "    {\n"
    "        finishInputPulse(INPUT_STATE_ABORTED);\n"
    "        return;\n"
    "    }\n\n"
    "    // Latched HID stays physically asserted until RELEASE_ALL/teardown.\n"
    "    // Do not run it through the timed pulse deadline, otherwise B would\n"
    "    // be released between Acro Bike bunny hops.\n"
    "    if(g_inputKind == INPUT_KIND_HID_LATCH)\n"
    "        return;\n\n"
    "    const u64 now = osGetTime();",
    "latched HID bypasses timed release",
)

bridge = replace_once(
    bridge,
    "                              INPUT_CAP_HID_ONLY_NO_IR | INPUT_CAP_LEGACY_OFF |\n"
    "                              INPUT_CAP_TOUCH_PULSE;",
    "                              INPUT_CAP_HID_ONLY_NO_IR | INPUT_CAP_LEGACY_OFF |\n"
    "                              INPUT_CAP_TOUCH_PULSE | INPUT_CAP_HID_LATCH;",
    "advertise latched HID capability",
)

handle_latch = r'''
static void handleInputHidLatch(int sock, const struct sockaddr_in *peer, socklen_t peerLen,
                                const BridgeRequest *req)
{
    serviceInputPulse();

    const u32 rawHid = req->argument;

    // Command 10 is deliberately simple:
    //   argument = one active-low non-neutral HID state
    //   aux      = 0 (no timer; RELEASE_ALL owns neutralization)
    if(req->requestId == 0 ||
       (rawHid & ~POKEBOT_INPUT_HID_NEUTRAL) != 0 ||
       rawHid == POKEBOT_INPUT_HID_NEUTRAL ||
       req->aux != 0)
    {
        sendInputStatus(sock, peer, peerLen, req, STATUS_INPUT_INVALID, -1,
                        req->requestId, INPUT_STATE_ABORTED);
        return;
    }

    // Sequence dedupe: a retry/status recovery for the SAME latch never
    // injects a second gameplay edge and never releases the currently held HID.
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
    g_inputKind = INPUT_KIND_HID_LATCH;
    g_inputSettleMs = 0;
    g_inputDeadlineMs = 0;

    sendInputStatus(sock, peer, peerLen, req, STATUS_OK, 0,
                    req->requestId, INPUT_STATE_ACCEPTED);
}

'''

bridge = replace_once(
    bridge,
    "static void handleInputStatus(int sock, const struct sockaddr_in *peer, socklen_t peerLen,",
    handle_latch +
    "static void handleInputStatus(int sock, const struct sockaddr_in *peer, socklen_t peerLen,",
    "latched HID handler insertion",
)

bridge = replace_once(
    bridge,
    "        case CMD_INPUT_TOUCH_PULSE:\n"
    "            handleInputTouchPulse(sock, peer, peerLen, &req);\n"
    "            break;\n"
    "        case CMD_INPUT_STATUS:",
    "        case CMD_INPUT_TOUCH_PULSE:\n"
    "            handleInputTouchPulse(sock, peer, peerLen, &req);\n"
    "            break;\n"
    "        case CMD_INPUT_HID_LATCH:\n"
    "            handleInputHidLatch(sock, peer, peerLen, &req);\n"
    "            break;\n"
    "        case CMD_INPUT_STATUS:",
    "latched HID command dispatch",
)

required = [
    "#define CMD_INPUT_HID_LATCH   10u",
    "#define INPUT_CAP_HID_LATCH",
    "#define INPUT_RUNTIME_HID_LATCHED",
    "#define INPUT_KIND_HID_LATCH",
    "if(g_inputKind == INPUT_KIND_HID_LATCH)",
    "static void handleInputHidLatch",
    "g_inputKind = INPUT_KIND_HID_LATCH;",
    "case CMD_INPUT_HID_LATCH:",
]
for marker in required:
    if marker not in bridge:
        raise SystemExit(f"v0p6 transform missing marker: {marker}")

# Frozen behavior markers for the already-proven commands must remain.
frozen = [
    "case CMD_INPUT_PULSE:",
    "handleInputPulse(sock, peer, peerLen, &req);",
    "case CMD_INPUT_TOUCH_PULSE:",
    "handleInputTouchPulse(sock, peer, peerLen, &req);",
    "case CMD_RELEASE_ALL:",
    "handleReleaseAll(sock, peer, peerLen, &req);",
]
for marker in frozen:
    if marker not in bridge:
        raise SystemExit(f"v0p6 froze command marker missing: {marker}")

bridge_path.write_text(bridge, encoding="utf-8")
print("v0p6 latched HID command 10 transform applied")
