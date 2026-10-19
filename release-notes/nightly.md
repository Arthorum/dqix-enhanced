**The newest work, less tested.** Use the stable release (v28) for your main
adventure; try this one in its own folder with a copy of your save.

**New since stable v28**
- **Sharper Enhanced mode**: the 3D resolution goes up to 12x, and the new
  **Auto** setting (Graphics > 3D resolution, F4) renders at one and a half
  times the size the game is shown at, so fullscreen on a big monitor is
  never stretched and edges come out smooth. If you had 4x (the most
  before), you now have Auto. Upscaling also keeps every pixel the same size
  at any window size (it made some rows wider than others before).
- **Smooth textures** (Graphics menu, on at first in Enhanced): the DS
  shows every texel of a texture as a block, which at high resolutions
  looks like a low-resolution picture on sharp 3D. Now textures are
  smoothed, and cut-out shapes (leaves, grass, hair) get smooth edges.
  Turn it off for the DS's blocky look at high resolution.
- **Widescreen sky**: skies drawn as a backdrop behind everything (the top
  of the Observatory) now fill the whole wide picture instead of only the
  middle, with the game's bare backup sky showing at the sides.
- **Widescreen with a clean field**: View > *Widescreen: the small screen in
  its own window* puts the map (and the battle commands) in a second window,
  so the field fills the first one with nothing over it.
- **Two windows, both fullscreen**: the second window has its own bar with
  *Fullscreen*, so each screen can fill its own monitor (F11 also works in
  either window; move the mouse to the top to get the bar back).
- **Screens your way** (View menu):
  - **Swap the screens** (F9): in stacked, side by side and top screen in
    the corner layouts, the two screens trade places.
  - **Two windows**: one screen in each, to put them on two monitors. The
    second window opens on your other monitor and remembers where you leave
    it; F11 in either window makes that one fullscreen. Also in the F2 cycle
    when you have two monitors.
  - **Widescreen battles**: the command screen now sits in the top left
    corner, over the sky, instead of in front of your party and the enemies.
  - **See-through small screen** (O, widescreen): 100%, 75%, 50% or 25%. It
    turns solid while the mouse is on it, so you can still click it.
- **Enhanced mode looks like the DS again where it didn't**: see-through
  effects now follow the DS's own rules, so clouds are soft and wispy instead
  of solid white blocks, the glow around Yggdrasil is a soft halo again, and
  light beams, water and other glows look as they should.
  - Fog (the distance haze) no longer covers see-through effects in front of
    the sky, and it is no longer a step too thick everywhere.
  - Overlapping see-through layers no longer pile up into a solid colour or
    hide the sky behind them, and a cloud or glow no longer blends over
    itself.
  - Outlines: at 1x soft like on the DS; at higher resolutions thinner, so
    characters' arms and legs keep their colour instead of turning into
    outline (or, in v29-nightly1, looking see-through).
  - Solid parts of models drawn with see-through edges now hide what's
    behind them, as on the DS.
  - The DS's shadow effect only darkens where it should.
  Checked against the exact DS picture at the top of the Observatory: the
  difference went down by about three quarters.

**How to install**: download *Source code (zip)* below, unzip it and
double-click `Install Dragon Quest IX.bat`. Already playing? Use Help >
Check for updates instead.
