**Kept for the record.** A nightly version from 2026-10-17: the newest work of its day, less tested. To play, use the newest release.

**The other players' DSes on one thread (dqixcore library copies)**

The game is also built as a library (CORE=dqixcore.dll); in a session each
other player's DS runs from its own copy of it (its own memory), all on one
thread of the player's process, switching fibers whenever one waits. A
4-player session then needs ~2 cores instead of 4 busy processes fighting
for them (3 consoles on 2 cores: 2m20s instead of >25 min). hosting.c gives
each copy its settings, exit and yield; the ROM is shared. Processes remain
the fallback. Tests: DQIX_SESSION=local:N with DQIX_INPUT_1..3.
