**Kept for the record.** A nightly version from 2026-10-18: the newest work of its day, less tested. To play, use the newest release.

**Sessions stay in sync after a hiccup; session log**

A console with its radio off doesn't wait for the others, so frames sent
meanwhile could still arrive once its radio came back on and be heard or
not depending on timing: PCs drifted apart after a pause of a few seconds
in tag mode (then wireless froze on both ends). Frames that reached a
console while its radio was off are now never heard.

Also: session/front.log (progress, sync checks, what a stuck console waits
for, network), the title bar names the DS being waited for, build
fingerprints (log only), sessions don't write wireless.bin, build-pc.bat
builds dqixcore.dll.
