# Roadmap

What's planned for DQIX Enhanced, roughly in order. Done items move to the
bottom with the version they arrived in.

## Before the next stable release (v27)

- [ ] **A real internet session with friends**: 3-4 players, tag mode,
      co-op through Pavo and a DQVC purchase, on different home networks.
- [ ] **The host window on Windows**: check the address, the Copy button, the
      player list and Start the game.
- [ ] **The updater's first real run**: *Help > Check for updates* against
      the public repository, then an actual update and a rollback.
- [ ] **Release v27 as stable**, so players on stable get update notices too.

## Multiplayer

The original game never had these, because its wireless only reached DSes in
the same room.

- [ ] **Join a session that has already started.**
- [ ] **A way in for friends whose router won't open the port**, without
      everyone installing a virtual LAN.
- [ ] **Lower input delay when the DSes aren't talking wirelessly**, so walking
      around during a session feels as responsive as playing alone.

## Content

- [ ] **Hand-written weekly DQVC news**: one message per sale, written by hand
      in Sellma's voice, replacing the single surviving message from April
      2010. The game turns the text into the news download.
- [ ] **Optional: the downloadable quests one per week**, in the order Nintendo
      released them, instead of all at once.

## Look and feel

- [ ] **Fine-tune the DS Lite screen filter** against photos of a real DS Lite
      running DQIX (colours per backlight level, pixel grid, motion blur).
- [ ] **Enhanced extras**: widescreen, and a map on the top screen.
- [ ] **Save states.**

## Reach

- [ ] **European and Japanese ROMs** in BUILD.bat (only the USA version builds
      today).

## Loose ends

- [ ] Find out why one test save never canvasses another in tag mode (every
      other pairing works; probably something in those two saves).
- [ ] Speed: 528 small jump stubs in the game's code still go through a
      slower path.

## Done

- [x] DQVC and the downloadable quests built into the game; no extra files
      needed (v27-nightly10).
- [x] In-game updates from GitHub, stable or nightly (v27-nightly9).
- [x] Every player gets their own daily DQVC stock; visitors can shop at the
      host's Sellma (v27-nightly8).
- [x] 3 and 4 players in co-op and tag mode (v27-nightly7, nightly8).
- [x] DQVC on the original calendar: sales start on Fridays at 08:00 UTC, new
      stock every day (v27-nightly6).
- [x] DQVC inside multiplayer sessions; the host window; the DS Lite photo as
      the default console (v27-nightly5).
- [x] Sessions stay in sync after a network hiccup; a session log for bug
      reports (v27-nightly4).
- [x] Playing with friends far away: tag mode and co-op over the internet
      (v27-nightly2, nightly3).
