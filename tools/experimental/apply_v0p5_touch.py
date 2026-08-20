#!/usr/bin/env python3
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly 1 match, found {count}")
    return text.replace(old, new, 1)


# ---------------------------------------------------------------------------
# Backend: extend the internally-owned InputRedirection packet sender so the
# acknowledged bridge can set touch as well as HID. Circle pad remains neutral.
# ---------------------------------------------------------------------------

header_path = Path("sysmodules/rosalina/include/pokebot_input.h")
header = header_path.read_text(encoding="utf-8")
header = replace_once(
    header,
    "void PokebotInput_SetRawHid(u32 rawHid);\nvoid PokebotInput_ReleaseAll(void);",
    "void PokebotInput_SetRawHid(u32 rawHid);\n"
    "void PokebotInput_SetTouchState(u32 touchState);\n"
    "void PokebotInput_ReleaseAll(void);",
    "header touch setter",
)
header_path.write_text(header, encoding="utf-8")

input_path = Path("sysmodules/rosalina/source/pokebot_input.c")
input_text = input_path.read_text(encoding="utf-8")

input_text = replace_once(
    input_text,
    "static Result sendInputPacket(u32 rawHid)",
    "static Result sendInputPacket(u32 rawHid, u32 touchState)",
    "backend packet signature",
)
input_text = replace_once(
    input_text,
    "        POKEBOT_INPUT_TOUCH_NEUTRAL,\n        POKEBOT_INPUT_CIRCLE_NEUTRAL,",
    "        touchState,\n        POKEBOT_INPUT_CIRCLE_NEUTRAL,",
    "backend touch packet field",
)

neutral_old = "sendInputPacket(POKEBOT_INPUT_HID_NEUTRAL);"
neutral_count = input_text.count(neutral_old)
if neutral_count < 3:
    raise SystemExit(
        f"backend neutral packet calls: expected at least 3 matches, found {neutral_count}"
    )
input_text = input_text.replace(
    neutral_old,
    "sendInputPacket(POKEBOT_INPUT_HID_NEUTRAL, POKEBOT_INPUT_TOUCH_NEUTRAL);",
)

input_text = replace_once(
    input_text,
    "    (void)sendInputPacket(rawHid);",
    "    (void)sendInputPacket(rawHid, POKEBOT_INPUT_TOUCH_NEUTRAL);",
    "backend HID setter",
)

input_text = replace_once(
    input_text,
    "void PokebotInput_SetRawHid(u32 rawHid)\n"
    "{\n"
    "    if(!PokebotInput_IsEnabled())\n"
    "        return;\n\n"
    "    (void)sendInputPacket(rawHid, POKEBOT_INPUT_TOUCH_NEUTRAL);\n"
    "}\n\n"
    "u32 PokebotInput_GetRawHid(void)",
    "void PokebotInput_SetRawHid(u32 rawHid)\n"
    "{\n"
    "    if(!PokebotInput_IsEnabled())\n"
    "        return;\n\n"
    "    (void)sendInputPacket(rawHid, POKEBOT_INPUT_TOUCH_NEUTRAL);\n"
    "}\n\n"
    "void PokebotInput_SetTouchState(u32 touchState)\n"
    "{\n"
    "    if(!PokebotInput_IsEnabled())\n"
    "        return;\n\n"
    "    (void)sendInputPacket(POKEBOT_INPUT_HID_NEUTRAL, touchState);\n"
    "}\n\n"
    "u32 PokebotInput_GetRawHid(void)",
    "backend touch setter implementation",
)

# Every call must now pass both HID and touch state.
for line_no, line in enumerate(input_text.splitlines(), 1):
    if "sendInputPacket(" in line and "static Result sendInputPacket" not in line:
        if line.count(",") == 0:
            raise SystemExit(
                f"backend old one-argument sendInputPacket remains at line {line_no}: {line}"
            )

input_path.write_text(input_text, encoding="utf-8")


# ---------------------------------------------------------------------------
# Bridge: additive command 9 INPUT_TOUCH_PULSE.
# Wire request/response structs stay unchanged.
# argument = encoded InputRedirection touch state (pressed state only)
# aux      = low16 hold_ms | high16 settle_ms
# ---------------------------------------------------------------------------

bridge_path = Path("sysmodules/rosalina/source/pokebot_bridge.c")
bridge = bridge_path.read_text(encoding="utf-8")

bridge = replace_once(
    bridge,
    "#define CMD_RELEASE_ALL  8u",
    "#define CMD_RELEASE_ALL       8u\n#define CMD_INPUT_TOUCH_PULSE 9u",
    "touch command define",
)

bridge = replace_once(
    bridge,
    "#define INPUT_CAP_LEGACY_OFF      (1u << 5)",
    "#define INPUT_CAP_LEGACY_OFF      (1u << 5)\n"
    "#define INPUT_CAP_TOUCH_PULSE     (1u << 6)",
    "touch capability define",
)

bridge = replace_once(
    bridge,
    "#define INPUT_RUNTIME_PULSE_ACTIVE  (1u << 2)",
    "#define INPUT_RUNTIME_PULSE_ACTIVE  (1u << 2)\n"
    "#define INPUT_RUNTIME_TOUCH_ACTIVE  (1u << 3)",
    "touch runtime define",
)

bridge = replace_once(
    bridge,
    "#define INPUT_STATE_NOT_FOUND         6u",
    "#define INPUT_STATE_NOT_FOUND         6u\n\n"
    "#define INPUT_KIND_NONE  0u\n"
    "#define INPUT_KIND_HID   1u\n"
    "#define INPUT_KIND_TOUCH 2u",
    "input kind defines",
)

bridge = replace_once(
    bridge,
    "static u32 g_inputRawHid = POKEBOT_INPUT_HID_NEUTRAL;\n"
    "static u32 g_inputSettleMs = 0;",
    "static u32 g_inputRawHid = POKEBOT_INPUT_HID_NEUTRAL;\n"
    "static u32 g_inputKind = INPUT_KIND_NONE;\n"
    "static u32 g_inputSettleMs = 0;",
    "input kind state",
)

bridge = replace_once(
    bridge,
    "    if(g_inputActive)\n"
    "        flags |= INPUT_RUNTIME_PULSE_ACTIVE;\n"
    "    return flags;",
    "    if(g_inputActive)\n"
    "        flags |= INPUT_RUNTIME_PULSE_ACTIVE;\n"
    "    if(g_inputActive && g_inputKind == INPUT_KIND_TOUCH)\n"
    "        flags |= INPUT_RUNTIME_TOUCH_ACTIVE;\n"
    "    return flags;",
    "touch runtime flag",
)

bridge = replace_once(
    bridge,
    "    g_inputRawHid = POKEBOT_INPUT_HID_NEUTRAL;\n"
    "    g_inputSettleMs = 0;",
    "    g_inputRawHid = POKEBOT_INPUT_HID_NEUTRAL;\n"
    "    g_inputKind = INPUT_KIND_NONE;\n"
    "    g_inputSettleMs = 0;",
    "finish pulse resets input kind",
)

bridge = replace_once(
    bridge,
    "                              INPUT_CAP_HID_ONLY_NO_IR | INPUT_CAP_LEGACY_OFF;",
    "                              INPUT_CAP_HID_ONLY_NO_IR | INPUT_CAP_LEGACY_OFF |\n"
    "                              INPUT_CAP_TOUCH_PULSE;",
    "advertise touch capability",
)

bridge = replace_once(
    bridge,
    "    g_inputRawHid = rawHid;\n"
    "    g_inputSettleMs = settleMs;",
    "    g_inputRawHid = rawHid;\n"
    "    g_inputKind = INPUT_KIND_HID;\n"
    "    g_inputSettleMs = settleMs;",
    "mark HID pulse kind",
)

handle_touch = r'''
static void handleInputTouchPulse(int sock, const struct sockaddr_in *peer, socklen_t peerLen,
                                  const BridgeRequest *req)
{
    serviceInputPulse();

    const u32 touchState = req->argument;
    const u32 holdMs = req->aux & 0xFFFFu;
    const u32 settleMs = (req->aux >> 16) & 0xFFFFu;

    // InputRedirection pressed touch format is:
    //   bit24 set + 12-bit Y + 12-bit X
    // Neutral/release is deliberately not accepted as a pulse; RELEASE_ALL owns
    // neutralization and pulse completion always returns touch to neutral.
    if(req->requestId == 0 ||
       (touchState & 0xFF000000u) != 0x01000000u ||
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

    PokebotInput_SetTouchState(touchState);
    g_inputActive = true;
    g_inputSettling = false;
    g_inputSequenceId = req->requestId;
    g_inputRawHid = POKEBOT_INPUT_HID_NEUTRAL;
    g_inputKind = INPUT_KIND_TOUCH;
    g_inputSettleMs = settleMs;
    g_inputDeadlineMs = osGetTime() + holdMs;

    sendInputStatus(sock, peer, peerLen, req, STATUS_OK, 0,
                    req->requestId, INPUT_STATE_ACCEPTED);
}

'''

bridge = replace_once(
    bridge,
    "static void handleInputStatus(int sock, const struct sockaddr_in *peer, socklen_t peerLen,",
    handle_touch +
    "static void handleInputStatus(int sock, const struct sockaddr_in *peer, socklen_t peerLen,",
    "touch handler insertion",
)

bridge = replace_once(
    bridge,
    "        case CMD_INPUT_STATUS:\n"
    "            handleInputStatus(sock, peer, peerLen, &req);\n"
    "            break;",
    "        case CMD_INPUT_TOUCH_PULSE:\n"
    "            handleInputTouchPulse(sock, peer, peerLen, &req);\n"
    "            break;\n"
    "        case CMD_INPUT_STATUS:\n"
    "            handleInputStatus(sock, peer, peerLen, &req);\n"
    "            break;",
    "touch command dispatch",
)

# Guard against accidental duplicate or partial transforms.
required = [
    "#define CMD_INPUT_TOUCH_PULSE 9u",
    "#define INPUT_CAP_TOUCH_PULSE",
    "PokebotInput_SetTouchState(touchState);",
    "case CMD_INPUT_TOUCH_PULSE:",
    "g_inputKind = INPUT_KIND_TOUCH;",
]
for marker in required:
    if marker not in bridge:
        raise SystemExit(f"bridge transform missing marker: {marker}")

bridge_path.write_text(bridge, encoding="utf-8")
print("v0p5 acknowledged touchscreen pulse edits applied")
