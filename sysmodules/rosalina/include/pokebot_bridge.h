/*
 * Pokebot3DS read-only Rosalina bridge.
 *
 * This file is part of the SHGliscor/Pokebot-CFW modifications to Nexus3DS.
 * Nexus3DS/Luma3DS licensing terms continue to apply to the surrounding work.
 */
#pragma once

#include "MyThread.h"

#define POKEBOT_BRIDGE_PORT 4952
#define POKEBOT_BRIDGE_MAX_READ 512

MyThread *PokebotBridge_CreateThread(void);
void PokebotBridge_ThreadMain(void);
