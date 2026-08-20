# Pokebot3DS standalone Luma sysmodule v0p1

Experimental proof-of-concept for running the Pokebot3DS RAM bridge and acknowledged input controller **without replacing stock Luma3DS `boot.firm`**.

This branch does not replace or modify the hardware-proven Nexus `boot.firm` baseline.

## Critical HID safety rule

The background CXI may be started before the game, but **it does not patch HID at startup**.

State flow:

```text
module launched
  -> UDP 4952 RAM/network service available
  -> HID untouched / physical controls normal
  -> Alpha Sapphire or Omega Ruby process confirmed
  -> PC explicitly sends INPUT_ARM
  -> HID hook is installed with remote state neutral (0xFFF)
  -> acknowledged INPUT_PULSE commands allowed
  -> INPUT_DISARM / Stop
  -> RELEASE_ALL / neutral
  -> HID hook removed
```

`INPUT_PULSE` is rejected with `INPUT_NOT_ARMED` until `INPUT_ARM` succeeds.
`INPUT_ARM` is rejected with `GAME_NOT_READY` unless a supported ORAS process is already running.

This deliberately prevents the module from taking HID ownership during HOME Menu / early boot, which is intended to avoid the earlier class of failures where normal 3DS inputs stopped working.

Soft resets inside an active hunt do **not** automatically patch/unpatch HID every cycle. Arming is a bot-session boundary, not a reset-cycle boundary.

## Files

- `000401300000F902.cxi` — external Luma sysmodule
- `Pokebot3DS-Sysmodule-Launcher.3dsx` — one-shot Homebrew Launcher bootstrap
- `pokebot_sysmodule_probe_v0p1.py` — standalone PC proof tool

Custom sysmodule Title ID:

```text
000401300000F902
```

The low byte remains `02` for NATIVE_FIRM core version compatibility.

## Installation for the proof

1. Keep the normal/stock Luma3DS `boot.firm` unchanged.
2. In Luma configuration, enable **loading external FIRMs and modules**.
3. Copy:

```text
/luma/sysmodules/000401300000F902.cxi
```

4. Copy the launcher to, for example:

```text
/3ds/Pokebot3DS-Sysmodule-Launcher/Pokebot3DS-Sysmodule-Launcher.3dsx
```

5. Fully boot the 3DS normally.
6. Run **Pokebot3DS-Sysmodule-Launcher** from Homebrew Launcher once.
7. Return to HOME Menu.
8. Confirm physical buttons still work normally **before starting the game**.
9. Boot Pokémon Alpha Sapphire.
10. Leave Rosalina InputRedirection **OFF** for this proof.
11. From the PC run:

```text
python pokebot_sysmodule_probe_v0p1.py <3DS-IP> smoke
```

The smoke test performs:

```text
PING
INPUT_PING (must show unarmed / HID disabled)
GAME_INFO
INPUT_ARM
one A pulse: 300 ms hold + 120 ms settle
wait for COMPLETED
duplicate same sequence (must not press A again)
RELEASE_ALL
INPUT_DISARM
INPUT_PING (must show unarmed / HID disabled again)
```

## First hardware proof order

Do not jump directly into an automated starter loop.

1. Launcher succeeds.
2. HOME Menu physical controls still work.
3. UDP `ping` works before the game.
4. `input-ping` shows `armed=false`, `hid_enabled=false`.
5. Boot Alpha Sapphire.
6. `info` returns the game PID/title.
7. `arm` succeeds.
8. One visible A pulse succeeds.
9. Duplicate sequence does not create a second press.
10. `disarm` succeeds.
11. Physical controls still work after disarm.
12. Then 100 harmless L pulses.
13. Then 500 harmless L pulses.
14. Only after that: one Torchic reset cycle.

## Protocol additions

Existing UDP 4952 commands 1-8 remain unchanged.

New commands:

```text
9  INPUT_ARM
10 INPUT_DISARM
```

New status values:

```text
16 INPUT_NOT_ARMED
17 GAME_NOT_READY
```

Input capability flags additionally advertise:

```text
bit 6 EXPLICIT_ARM
bit 7 DIRECT_HID
```

## RAM safety

The RAM side remains read-only:

- no GDB attachment
- no game-memory write command
- page-mapped bounded reads only
- maximum read length 512 bytes

Supported bridge title IDs in this first source are:

```text
Omega Ruby     000400000011C400
Alpha Sapphire 000400000011C500
```

The current Pokebot3DS-CFW product remains Alpha Sapphire-only until other game mappings are separately proven.

## Experimental status

This source is intentionally separate from the proven custom Nexus controller branch. Do not merge it into the production firmware path based only on CI success. Hardware validation is required because HID patch timing and external-sysmodule launch behaviour are the purpose of this experiment.
