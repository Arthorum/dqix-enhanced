**Kept for the record.** A nightly version from 2026-10-18: the newest work of its day, less tested. To play, use the newest release.

**3 and 4 players can get together in co-op**

A host answers friends joining only between its beacon and the post-beacon
interrupt (about 9 ms). A joining DS answers each step about a frame later
than a real one here, so once one friend was in, the next one's requests
kept missing the window and the join failed. A host's post-beacon window is
now 24 ms longer. Tested with four different saves: 3 and 4 players meet in
the host's world; tag mode between four saves.
