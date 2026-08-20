#!/usr/bin/env python3
from pathlib import Path

path = Path("sysmodules/rosalina/source/pokebot_bridge.c")
text = path.read_text(encoding="utf-8")

replacements = [
    (
        "void PokebotBridge_ThreadMain(void)\n{\n    PokebotInput_ReleaseAll();\n\n    if(g_scratch == NULL)",
        "void PokebotBridge_ThreadMain(void)\n{\n    if(g_scratch == NULL)",
    ),
    (
        "        while(!preTerminationRequested)\n        {\n            serviceInputPulse();\n\n            struct pollfd pfd;",
        "        while(!preTerminationRequested)\n        {\n            if(g_inputActive)\n                serviceInputPulse();\n\n            struct pollfd pfd;",
    ),
    (
        "            // 10 ms wake interval bounds pulse-release timing without polling\n            // game RAM. Network work remains event driven.\n            int pollRes = socPoll(&pfd, 1, 10);",
        "            // Preserve the hardware-proven RAM bridge's 50 ms idle network\n            // cadence. Controller servicing is only active after a pulse.\n            int pollRes = socPoll(&pfd, 1, 50);",
    ),
    (
        "        if(g_inputActive)\n            finishInputPulse(INPUT_STATE_ABORTED);\n        else\n            PokebotInput_ReleaseAll();\n\n        PokebotInput_Disable();\n        socClose(sock);",
        "        if(g_inputActive)\n            finishInputPulse(INPUT_STATE_ABORTED);\n\n        if(PokebotInput_IsEnabled())\n            PokebotInput_Disable();\n        socClose(sock);",
    ),
    (
        "    PokebotInput_ReleaseAll();\n    PokebotInput_Disable();\n    MyThread_Exit();",
        "    if(g_inputActive)\n        finishInputPulse(INPUT_STATE_ABORTED);\n    if(PokebotInput_IsEnabled())\n        PokebotInput_Disable();\n    MyThread_Exit();",
    ),
]

for index, (old, new) in enumerate(replacements, 1):
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"v0p4 replacement {index} expected exactly 1 match, found {count}")
    text = text.replace(old, new, 1)

path.write_text(text, encoding="utf-8")
print("v0p4 idle-safe bridge edits applied")
