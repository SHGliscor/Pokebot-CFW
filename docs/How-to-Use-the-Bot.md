# How to Use Pokebot-CFW

> **Current scope:** this guide covers the RAM-authoritative Qt starter-hunting bot as of **v0p19 and later**. The currently hardware-validated starter backend is for **Pokémon Alpha Sapphire** and supports **Treecko, Torchic, and Mudkip**.

## What this bot does

Pokebot-CFW is a capture-free shiny-hunting bot for a CFW Nintendo 3DS.

The bot uses:

- a read-only RAM bridge built into the custom Pokebot-CFW/Luma firmware;
- Luma **InputRedirection** for controller input;
- RAM-decoded PK6 data for species, PID, TID/SID, IVs, nature, shiny value and shiny status;
- a Windows Qt dashboard for hunt control, stats, party display, Last Seen history and support exports.

The capture card is **optional** and is not used to decide whether a Pokémon is shiny.

### Safety rules

The hunt loop is intentionally conservative:

- valid expected non-shiny PK6 → next reset is allowed;
- shiny → **absolute HOLD**;
- wrong species → HOLD;
- invalid checksum → HOLD;
- TID/SID mismatch → HOLD;
- invalid RAM state → HOLD;
- RAM read failure after bounded transport recovery → HOLD.

The bot does **not** write to game RAM.

---

# 1. Requirements

You need:

- a CFW Nintendo 3DS/New 3DS running the Pokebot-CFW `boot.firm`;
- Pokémon Alpha Sapphire;
- Luma InputRedirection enabled;
- the 3DS and Windows PC on the same local network;
- Python 3 on the Windows PC;
- the latest Pokebot-CFW Qt bot ZIP;
- PySide6, installed from the included `requirements.txt`.

A capture card is not required.

---

# 2. Install the Pokebot-CFW firmware

1. Power off the 3DS.
2. Back up the existing `boot.firm` from the root of the SD card.
3. Copy the Pokebot-CFW `boot.firm` to the root of the SD card.
4. Reinsert the SD card and boot the 3DS normally.

The custom firmware provides the bot's **read-only RAM bridge**, normally on UDP port **4952**.

Do not use a normal stock Luma `boot.firm` while expecting the RAM bridge to work.

---

# 3. Enable InputRedirection

On the 3DS:

1. Open the Rosalina menu with `L + D-Pad Down + SELECT`.
2. Open the miscellaneous options menu.
3. Start/enable **InputRedirection**.
4. Return to the game.

InputRedirection normally uses UDP port **4950**.

> InputRedirection does not provide a positive handshake back to the PC. The dashboard therefore reports it as **Configured**, not falsely as a confirmed connection.

---

# 4. Prepare Alpha Sapphire

Before starting a starter hunt:

1. Save the game at the calibrated Professor Birch bag position used by the starter bot.
2. The player should be directly in front of the bag and facing it.
3. Do not move the player away from that calibrated position after starting the bot.
4. Leave the game running normally on the 3DS.

The bot's reset route is RAM-gated and handles:

- game reset;
- title/continue flow;
- communication-error dismissal when present;
- return to the field;
- final Birch bag authority validation;
- starter selection;
- battle entry;
- authoritative PK6 read.

---

# 5. Set up the Windows bot

1. Extract the latest bot ZIP to a normal folder on the PC.
2. Open that folder.
3. Run:

```text
RUN_QT_LIVE.bat
```

If PySide6 is missing, open Command Prompt in the bot folder and run:

```text
py -3 -m pip install -r requirements.txt
```

Then run `RUN_QT_LIVE.bat` again.

The only current Python GUI dependency is:

```text
PySide6>=6.7,<7
```

---

# 6. Configure the 3DS connection

Open the **SETTINGS** tab.

Set:

- **3DS IP Address** — the current LAN IP of your 3DS;
- **RAM Bridge Port** — normally `4952`;
- **InputRedirection Port** — normally `4950`;
- **RAM Bridge Timeout** — leave at the default unless diagnosing network instability.

Then press **SAVE SETTINGS**.

Connection changes apply to the **next connection test or next hunt**.

Use **TEST CONNECTION** to verify the RAM bridge.

A successful connection test means the dashboard can identify the running Alpha Sapphire process and perform bounded RAM reads.

### If the connection test fails

Check:

- the custom Pokebot-CFW `boot.firm` is installed;
- the 3DS and PC are on the same network;
- the IP address in Settings is correct;
- the 3DS has not received a new DHCP address;
- the RAM bridge port is `4952` unless deliberately changed;
- wireless client/AP isolation is not blocking devices from talking to each other.

---

# 7. Start a starter hunt

On the **DASHBOARD**:

1. Select **Treecko**, **Torchic**, or **Mudkip**.
2. Confirm the connection status is ready.
3. Press **START HUNT**.

The selected starter modules are isolated from one another. A Treecko-specific change should not alter Torchic or Mudkip behavior.

The currently validated starter controllers have each passed dedicated 10/10 hardware validation.

Once running, do not manually press 3DS buttons or move the player unless you are deliberately stopping/recovering from a HOLD.

---

# 8. What happens during each attempt

A normal non-shiny cycle is approximately:

1. reset the game;
2. reacquire the Alpha Sapphire process;
3. pass the title/continue RAM gates;
4. dismiss the communication-error screen if necessary;
5. validate the exact Birch bag position/state;
6. open the starter chooser;
7. move to the selected starter;
8. confirm the starter;
9. enter battle;
10. wait for the authoritative battle RAM state;
11. read the starter's PK6 from RAM;
12. validate checksum, species and trainer identity;
13. calculate shiny XOR;
14. non-shiny → authorize the next reset;
15. shiny → HOLD immediately.

The bot does not continuously poll RAM. Reads are bounded and state-driven.

---

# 9. RAM transport recovery

UDP can occasionally lose a packet.

The Qt RAM wrapper therefore gives each logical RAM read **one bounded transport retry** on `TimeoutError`:

- same RAM address;
- same requested length;
- short delay;
- maximum two transport attempts total.

This is transport recovery, not continuous polling.

A retry is logged as:

```text
RAM_READ_TRANSPORT_RETRY
```

If the retry also fails, the bot safety-holds.

The reset route also contains bounded handling for the communication-error screen and final bag validation.

---

# 10. Shiny detection and shiny HOLD

Shiny detection comes only from validated PK6 RAM data.

When a shiny is found:

- the bot records the shiny encounter;
- lifetime/phase statistics update;
- the current phase ends;
- the selected starter's next phase begins;
- **no further reset is sent**;
- the dashboard enters **SHINY HOLD**;
- the configured shiny notification sound plays;
- a support ZIP can be generated automatically.

The bundled notification is:

```text
assets/gen6_shiny_notification.wav
```

You can change the WAV file in **SETTINGS** and use **TEST SHINY SOUND** to preview it.

---

# 11. Stop the bot safely

Press **STOP** on the dashboard.

Stop is cooperative and RAM-safe.

If Stop is pressed during the reset route:

- the bot finishes returning to the exact safe bag gate;
- it stops before beginning another starter-selection sequence.

If Stop is pressed during starter selection or battle:

- the current authoritative PK6 check is allowed to complete;
- no next reset is sent.

Closing the app during an active hunt also requests a safe stop instead of blindly killing the sequence.

---

# 12. Session Stats

Session statistics are automatic.

### Session Time

- starts when **START HUNT** is pressed;
- updates while the hunt is running;
- freezes when **STOP** is pressed;
- pressing Start again resumes the existing session rather than clearing it.

### Encounters per hour

The displayed rate is based on completed authoritative encounters divided by active session time.

This means:

- the rate naturally decays between encounters;
- it jumps upward when another encounter completes;
- it freezes while the session is stopped.

### Phase

Each starter has its own phase data.

Hunting Torchic does not change Treecko or Mudkip's phase counters.

When a shiny is found for a starter, only that starter's phase is completed and advanced.

---

# 13. Party Pokémon panel

The **PARTY POKÉMON** panel shows six visual party slots.

A successful startup/connection probe performs one bounded six-slot party snapshot so Pokémon already in the party can appear while the bot is idle.

The panel also refreshes after authoritative encounters.

Each populated slot can show:

- Pokémon image;
- species;
- gender;
- shiny indicator.

Hover over a party card for:

- Nature;
- Hidden Power;
- IVs;
- EVs;
- Pokérus status;
- Gender;
- SV.

Party reads are UI telemetry only and do not control shiny/reset decisions.

---

# 14. Last Seen Pokémon

The **LAST SEEN POKÉMON** panel keeps the most recent authoritative encounters in a compact ledger:

```text
[sprite]  HP  ATK  DEF  SPA  SPD  SPE  SUM  SV
```

Newest encounters appear at the top.

Hover a row to see additional details such as:

- species;
- gender;
- nature;
- PID;
- full IV spread;
- IV sum;
- SV.

Last Seen history persists between bot launches.

---

# 15. Persistent stats and settings

Persistent hunt data is stored outside the versioned bot folder at:

```text
%APPDATA%\Pokebot-3DS\
```

Typical layout:

```text
Pokebot-3DS\
├─ settings.json
├─ stats\
│  ├─ treecko.json
│  ├─ torchic.json
│  └─ mudkip.json
└─ history\
   ├─ last_seen_oras.json
   └─ recent_shinies.json
```

This means you can download a new bot build, extract it to a new folder and keep the same lifetime statistics automatically.

Do **not** copy old stats into every new build folder.

The bot includes legacy migration logic that can import older runtime stats into AppData when the shared profile is first created.

---

# 16. Reset All Stats

Open the **HUNTS** tab and press:

```text
RESET ALL STATS
```

The button is blocked while a hunt is active and requires confirmation.

It resets:

- Treecko lifetime encounters/shinies/phase;
- Torchic lifetime encounters/shinies/phase;
- Mudkip lifetime encounters/shinies/phase;
- Last Seen history;
- recent shiny history;
- current dashboard session counters.

It does **not** delete:

- 3DS IP/settings;
- backend logs;
- raw PK6 evidence;
- support ZIPs.

Use **OPEN STATS FOLDER** in the HUNTS tab to open the shared AppData profile.

---

# 17. Settings reference

The SETTINGS tab currently includes:

### Connection

- 3DS IP Address;
- RAM Bridge port;
- InputRedirection port;
- RAM bridge timeout;
- automatic connection test on startup.

### Interface/startup

- remember selected starter;
- always keep dashboard on top;
- enable/disable ORAS sprite downloads/cache.

### Shiny/support

- enable shiny sound notification;
- choose shiny WAV file;
- test shiny sound;
- automatic support ZIP export;
- number of recent raw PK6 files to retain.

### Fixed safety settings

The safety rules are deliberately not user-disableable.

---

# 18. Support ZIPs

With automatic support ZIPs enabled, the bot can export a ZIP after:

- shiny HOLD;
- safety HOLD;
- manual safe stop.

The ZIP is created in the current bot folder and is named similar to:

```text
torchic_qt_support_YYYYMMDD_HHMMSS.zip
```

A support ZIP normally contains:

- session/event report;
- backend log;
- current persistent starter stats snapshot;
- recent raw PK6 evidence.

When reporting a problem, upload the newest support ZIP without editing it.

---

# 19. Common troubleshooting

## RAM Bridge shows offline/not ready

Check the custom `boot.firm`, 3DS IP, same-network connectivity and RAM bridge port.

## Inputs do nothing

Confirm Luma InputRedirection is enabled and the configured input port is correct.

Remember: InputRedirection has no positive acknowledgement, so the dashboard cannot prove it is listening before sending an authorized input.

## Party Pokémon stays Empty

Run **TEST CONNECTION**. A successful connection probe performs the idle party snapshot.

If the RAM bridge is not ready, the party panel cannot update.

## Last Seen stays empty

Last Seen only receives completed authoritative PK6 encounters. A hunt that safety-holds before the PK6 validation stage will not add a valid Last Seen encounter.

## Pokémon image is missing but text data is present

Sprite images are presentation-only. Make sure ORAS sprite downloading is enabled and the PC has internet access. RAM-derived data remains valid if the image download fails.

## Bot reports SAFETY HOLD

Do not treat the HOLD itself as the bug. HOLD is the protection working.

Upload the generated support ZIP so the exact failed RAM/state/transport gate can be diagnosed.

## `RAM_READ_TRANSPORT_RETRY` appears in the log

One UDP RAM-read request timed out and was retransmitted once. If the second attempt succeeds, the hunt continues. If it also fails, the bot safety-holds.

## `RESET_COMM_SECOND_DISMISS` appears

The communication-error screen remained present after the first RAM-gated dismissal, so the bot used its one additional allowed dismissal attempt.

---

# 20. Updating to a new bot build

When a new ZIP is released:

1. Stop the current hunt safely.
2. Close the old bot.
3. Extract the new ZIP to a new folder.
4. Run the new `RUN_QT_LIVE.bat`.
5. Your shared AppData settings and lifetime statistics should load automatically.
6. Run **TEST CONNECTION** before beginning a long hunt.

You do not need to move the AppData statistics folder into the new build.

Keep the previous bot folder until the new build has been tested successfully on hardware.

---

# 21. Current limitations

The current validated Qt backend is focused on Alpha Sapphire starter hunts.

Do not assume that other games, other ORAS regions/builds, wild hunts, static encounters or additional hunt types are supported until those modes have their own validated RAM gates and controllers.

The UI already contains tabs/placeholders for future expansion, but visible UI does not imply that an unfinished hunt backend is ready to use.

---

# Quick Start Checklist

- [ ] Pokebot-CFW `boot.firm` installed on the 3DS.
- [ ] Alpha Sapphire running.
- [ ] InputRedirection enabled.
- [ ] 3DS and PC on the same network.
- [ ] Latest bot ZIP extracted.
- [ ] PySide6 installed.
- [ ] Correct 3DS IP entered in SETTINGS.
- [ ] TEST CONNECTION passes.
- [ ] Player is at the calibrated Birch bag position.
- [ ] Correct starter selected.
- [ ] Shiny sound tested if desired.
- [ ] Press START HUNT.
- [ ] Use STOP for a cooperative safe stop.
- [ ] If a HOLD occurs, upload the generated support ZIP.
