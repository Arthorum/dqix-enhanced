The built-in Nintendo Wi-Fi Connection server (Wireless > Server > Built-in)
answers the game from the folders here, one per game code:

  YDQE  Dragon Quest IX (USA)      YDQP  (Europe)      YDQJ  (Japan)

Each holds DQIX's downloads as Nintendo's download server (dls1) last served
them, preserved by the community: https://github.com/InternalLoss/DLS1-Files
(collected from AltWFC and other servers). _list.txt lists them:

  file name <tab> <tab> attr1 <tab> attr2 <tab> attr3 <tab> size

  quest*     the downloadable quests (the last one switches on all of them)
  auction*   DQVC's stock
  infomes*   the news message

They are game data, so they are kept out of the git history; build-windows.sh
copies them next to dqix.exe.
