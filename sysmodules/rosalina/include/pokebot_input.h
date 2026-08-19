/*
 * Pokebot3DS HID-only controller backend.
 *
 * This is intentionally separate from legacy Luma InputRedirection. It reuses
 * the proven HID hook mechanism but does not start the UDP/4950 service and
 * does not patch IR. Legacy InputRedirection must remain disabled while this
 * backend owns the HID hook.
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
