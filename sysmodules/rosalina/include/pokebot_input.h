/*
 * Pokebot3DS acknowledged controller backend.
 *
 * v0p3 deliberately stops maintaining a second private HID hook. Instead it
 * owns the already-proven Nexus/Luma InputRedirection controller path
 * internally and feeds neutral/button packets to that path from the 4952
 * acknowledged bridge. This keeps the normal 4950 implementation intact while
 * avoiding duplicate HID patch ownership.
 */
#pragma once

#include <3ds/types.h>
#include <stdbool.h>

#define POKEBOT_INPUT_HID_NEUTRAL    0x00000FFFu
#define POKEBOT_INPUT_TOUCH_NEUTRAL  0x02000000u
#define POKEBOT_INPUT_CIRCLE_NEUTRAL 0x007FF7FFu

Result PokebotInput_Enable(void);
Result PokebotInput_Disable(void);
bool PokebotInput_IsEnabled(void);
bool PokebotInput_LegacyIsEnabled(void);
void PokebotInput_SetRawHid(u32 rawHid);
void PokebotInput_ReleaseAll(void);
u32 PokebotInput_GetRawHid(void);
