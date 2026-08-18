/* Start the Pokebot read-only bridge from Rosalina's existing constructor phase. */
#include "pokebot_bridge.h"

__attribute__((constructor))
static void PokebotBridge_AutoStart(void)
{
    PokebotBridge_CreateThread();
}
