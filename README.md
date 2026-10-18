# DQIX Enhanced

**Dragon Quest IX: Sentinels of the Starry Skies, as a native PC game.**

This project turns your own copy of Dragon Quest IX (the USA DS cartridge's
ROM) into a real Windows program, built on your PC from your ROM. Nothing
from the game is included here: no code, no graphics, no music. It isn't an
emulator running the DS's code. The game's code has been recompiled into
native PC code, with the DS's hardware rebuilt around it. So it can play
exactly like the original, and it can also do things the DS never could.

## Features

**The game as it was**
- **Classic mode**: every frame exactly as a DS draws it, checked pixel for
  pixel against a reference DS emulator, with the DS's own sound and timing.
- Your whole adventure: the story, alchemy, grottoes, Quester's Rest, and
  the downloadable quests and special guests.
- Saves in the DS's own format, so they work with save editors and tools.

**Better than the DS**
- **Enhanced mode**: the 3D drawn on your graphics card at up to 4x the DS's
  resolution. Switch between Classic and Enhanced at any time (F5).
- **The DS Lite look**: a screen filter with the DS Lite's colours, pixel
  grid, motion blur and backlight levels. The screens can sit inside a photo
  of a real DS Lite, or a drawn one in six colours, even at actual size.
- Any window size, fullscreen, screens stacked or side by side, and sharp
  pixel scaling.
- Keyboard, mouse (as the touch screen) and gamepads. Hold Tab to fast
  forward.

**Multiplayer, from anywhere**
- **Tag mode** (canvassing for guests) and **co-op** (visiting and inviting
  through Pavo) with up to 4 players, over the internet: friends don't need
  to be in the same room.
- The game sets up your router by itself where it can (UPnP), and the host
  gets a window with the address to send to friends.

**DQVC and downloads, offline**
- **Sellma's DQVC shop is back**, with the original 52 weekly sales on their
  original calendar. New stock arrives every day, every player gets their
  own shop, and friends visiting your world can shop at yours.
- The downloadable quests arrive through Sellma like in 2010, from a small
  server built into the game: no internet connection or extra files needed.
- An option to unlock all downloadable quests and special guests at once.

**Easy to keep up to date**
- The game tells you when a new version is out and updates itself
  (Help > Check for updates). Choose *stable* (tested) or *nightly* (the
  newest work).
- F10 saves a bug report with a screenshot and everything needed to
  replay what happened.

## Getting started

You need your own Dragon Quest IX cartridge's ROM (USA version, `.nds`),
Windows 10 or 11 (64-bit), about 3 GB of free space, and the internet the
first time.

1. Download the **[stable version](https://github.com/sackchief/dqix-enhanced/archive/refs/heads/stable.zip)**
   and unzip it somewhere, for example in Documents. (The green **Code**
   button gives the nightly version: the newest work, less tested.)
2. Put your ROM in the `rom` folder.
3. Double-click **`BUILD.bat`**. The first time, it installs free build tools
   (MSYS2), which takes 10-20 minutes in all. Later builds are much faster.
4. Play with **`game\play.bat`**. Make a desktop shortcut to it if you like.

Your save (`dqix.sav`) and settings (`settings.ini`) live in the `game`
folder. **Don't share the `game` or `build` folders**: they contain the game.

## Controls

| DS | Keyboard | Gamepad |
|---|---|---|
| D-pad | arrow keys | D-pad / left stick |
| A / B / X / Y | X / Z / S / A | right / bottom / top / left button |
| L / R | Q / W | shoulder buttons |
| START / SELECT | Enter / Right Shift | Start / Back |
| touch screen | left mouse button on the bottom screen | |

Every setting is in the **menu bar** at the top of the window. Handy keys:
Tab (hold) fast forward, P pause, F5 Classic / Enhanced, F6 DS Lite screen,
F7 DS Lite console, F11 fullscreen, F12 screenshot, F10 bug report.

## Playing with friends

Everyone needs the same version of the game.

1. Save your adventure first: the game restarts for a session.
2. One player hosts: *Wireless > Play with friends far away > Host*. A window
   shows the address to send to your friends, with a Copy button.
3. The others choose *Join a friend* and paste that address.
4. When everyone is in the host's list, the host presses **Start the game**.

Then play as you would with DSes side by side: canvass for guests with
Erinn, or visit and invite through Pavo.

If friends can't connect, the host's router didn't open the port by itself.
Either forward TCP port 27900 to the host's PC, or all join a free virtual
LAN (Tailscale, ZeroTier or Radmin VPN) and use the host's address there.

## Updating

The game checks for a new stable version when it starts and asks before
changing anything. *Help > Check for updates* shows both the stable and the
nightly version, so you can choose either. An update downloads the new
version of this project, builds the game again from your ROM, and restarts.
Your saves and settings are kept, and the previous version is kept
in the `update` folder (`update\rollback.bat` goes back to it).

## If something goes wrong

Press **F10** in the game: it saves a report in `game\reports\` with a box
to describe what happened. For multiplayer problems, also include
`game\session\front.log`.

## About this project

The function names and boundaries come from the public-domain Dragon Quest
IX decompilation project. The wireless chip, the 3D renderer and the sound
follow the melonDS emulator's research. How everything fits together, and
the tools for testing, are in [docs/TECHNICAL.md](docs/TECHNICAL.md).

Dragon Quest IX is © Square Enix / Level-5 / Armor Project / Bird Studio /
Koichi Sugiyama. This project isn't affiliated with or endorsed by Square
Enix or Nintendo. It contains no game files, and it only works with a ROM
you dumped from your own cartridge.

Licence: GPL-3.0-or-later (see `LICENSE`). It includes public-domain data
from the DQIX decompilation project (`config/`), libslirp (BSD,
`third_party/libslirp`) and stb_truetype (public domain), and translated
parts of melonDS (GPL-3.0-or-later).
