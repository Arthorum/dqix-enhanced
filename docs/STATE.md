# Where the project stands

Git tag `bookmark-2026-10-15` marks the last stable version (v26). To go back
to it: `git checkout bookmark-2026-10-15`.

## Stable and nightly

- **Stable** = git branch `stable` (now v26, tag `bookmark-2026-10-15`): the
  version to play your main save on. Changes only by promoting a proven
  nightly, or a small safe fix (made on `stable`, then merged into `main`).
  Keep it in its own folder, built with BUILD.bat.
- **Nightly** = branch `main`, versions `v27-nightly1`, `-nightly2`...
  Experimental (now: wireless multiplayer). Its own folder and saves.
- **Promoting**: when a nightly is solid, it becomes the next stable: set the
  version to `v27`, bookmark (tag), move `stable` to it, make the stable zips.

## What works (v26)

- **The whole game** recompiled to C (both CPUs) and running natively on
  Windows: graphics (Classic = pixel-exact DS, Enhanced = GPU 3D at 1-4x),
  sound, input (keyboard, mouse touch, gamepad), saves compatible with the DS.
- **Menu bar** (Game, Graphics, Screen, View, Wireless, Help); `settings.ini`.
- **DS Lite look**: LCD filter (colours, pixel grid, motion blur, backlight),
  drawn console in six colours, the Jet Black photo (`skins/`), actual size.
- **Bug reports** (F10, with a description box): screenshot, inputs, saves,
  log, perf; deterministic replay of the session (see README).
- **Wireless**: the DS Wi-Fi chip (from melonDS), the access point DQIX-PC,
  libslirp for the internet, and the **built-in Nintendo WFC server**
  (`runtime/wfc.c`): Sellma's download works offline, all 64 DLC quests,
  DQVC's 52 weekly sales on the original calendar (PC clock).
- **One-click build** from a ROM: `BUILD.bat` -> `game/`. Zips for sharing:
  `tools/make-release.py` -> `release/` (project; extras separately).

## Key facts found along the way

- DQIX download files: RC4, key `XENLONPROJECTKEY`; last 4 bytes make the
  encrypted file (big-endian number) divisible by 0xC2A030D4 (ov023 021f6324).
  Stock file = pool of `0x68` records (item, price, 1, qty); the game shows 6.
- Login needs a `Date:` header; NoSSL = https:// -> http:// in overlay 31 RAM.
- Save: two 32 KB slots, "DRAGON QUEST IX"; quest status nibbles at 12300
  (bit 3 = DLC received); guests bitmask at 11528; DQVC stock at 27840 (40-byte
  records); news text near 23188.
- DS firmware Wi-Fi slots at FW_USER-0x400; the game uses slot 1 ("DQIX-PC").
- The decomp's `config/usa/arm9` (public domain) + `tools/extract_rom.py`
  reproduce `dsd`'s extraction byte for byte; generated C is identical.

## Nightly: wireless between DSes and playing far apart (v27-nightly3)

- **The air** (`runtime/air.c`): copies of the game on one PC share memory;
  every console counts the same 8 us ticks, a frame arrives at its sender's
  tick + preamble, a listening console waits until the others are within 11
  ticks, frames are taken in (tick, console) order: deterministic.
- **The chip** (`runtime/wifi.c`): the multiplayer half added (melonDS
  Wifi.cpp): frames/beacons between consoles, MP CMD with reply windows,
  client auto-replies, ACK. Tested: tag mode (both counters go to 1, the
  other's title and name shown) and co-op through Pavo ("Player A has
  arrived!", guest in the host's world) between two copies.
- **Sessions** (`runtime/session.c`, `upnp.c`): every PC runs every player's
  console (others hidden: same exe, `DQIX_HIDDEN=1`, Windows job object kills
  them with the window); TCP via the host carries only buttons (lockstep, input
  delay from pings, 3+ frames) plus RAM fingerprints every 120 frames. Save +
  128 KB firmware of each player go to all at the start; RTC = host's. A
  player who leaves is switched off at the same frame everywhere
  (`AIR_INPUT_END`). Lobby on the DS screens (stb_truetype, system font),
  Wireless > Play with friends far away (Host / Join / Start / Leave / name).
  Tested: co-op between two simulated PCs over TCP, 87/87 fingerprints equal;
  windowed lobby + start under Xvfb.
- How DQIX's wireless starts: tag mode = Erinn > Canvass for guests > save >
  "Go get 'em!" > **A** (only then the radio comes on; it alternates parent
  (beacons) and scanning on channels 1/7/13 at random intervals). Co-op =
  Pavo (Quester's Rest, right end of the counter, 1st floor): host "Invite
  friends from other worlds", guest "Visit another world" (alone).
- **nightly3**: the other players' DSes run in the player's own process, on
  one thread, from copies of `dqixcore.dll` (the same objects linked as a
  library; `hosting.c`: per-copy settings instead of getenv, exit = this
  console only, yield = fiber switch, shared ROM). 3 consoles in tag mode on
  2 cores: fine (12000 frames in 2m20s headless) vs >25 min as processes.
  Fallback to processes if the library is missing. `DQIX_SESSION=local:N`
  tests N consoles on one PC.
- **nightly4**: fixed PCs drifting apart after a hiccup (a PC pausing a few
  seconds while a radio was on): a console with its radio off doesn't wait
  for the others, so frames sent during that time could still be in the
  air when its radio came back on, and were then heard or not depending on
  timing. Now frames that reached a console while its radio was off are
  never heard (`g_deaf_until` in air.c). Before: a 6 s pause in tag mode =
  out of sync within 20 s; after: 74/74 checks the same. Also:
  `session\front.log` (every console's progress every 10 s, each sync
  check, what a console is stuck waiting for after 1.5 s, the network), the
  title bar says what it's waiting for, build fingerprints in HELLO/WELCOME
  (log only), sessions no longer write `wireless.bin`, build-pc.bat builds
  dqixcore.dll too.
- Not done yet: 3-4 players tested only with one save (DQIX treats copies of
  the same save as one person: each tags 1); a relay for hosts who can't open
  a port (virtual LAN for now).

## Later ideas

- DLC quests one per week in their release order (quest file format known).
- DS Lite filter tuned against photos of a real DS Lite; save states;
  Enhanced extras (widescreen, top-screen map); EU/JP ROMs in BUILD.bat.
