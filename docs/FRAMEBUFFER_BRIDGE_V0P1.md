# Pokebot-CFW Framebuffer Bridge v0p1

Experimental read-only top-screen snapshot transport for Pokebot3DS-CFW. Existing commands 1-10 remain unchanged. The planned commands 11-13 capture a stable 400x240 top-screen image, read it in bounded chunks, and release it. This is visual telemetry only: RAM/PK6 remains shiny authority, and no memory-write primitive is added.

The hardware-test client will save the captured top screen as PNG. Discord integration comes only after standalone hardware proof.
