# DQIX static recompiler (private)

Turns Dragon Quest IX's DS machine code (both CPUs) into C that compiles
natively, plus the runtime that stands in for the DS hardware.

* Milestone 1 (done): the whole game recompiles, compiles and links.
* Milestone 2 (done): the game boots on PC with both CPUs.
* Milestone 3 (done): graphics. 2D engines and a software 3D renderer. Logos,
  opening movie, title, adventure-log menu, character creation, the story
  opening in Angel Falls and the first battle all render.
* Milestone 4 (done): a playable PC build. Window, keyboard / mouse / gamepad,
  sound, frame pacing (SDL2). Built on Windows with `build-pc.bat`.
* Milestone 5 (done): Classic mode, the game exactly as a DS shows and plays
  it (see "Classic and Enhanced" below), and Enhanced mode, with 3D drawn on
  the graphics card at 1-4x resolution.
* Next: more Enhanced features (widescreen, top-screen map), always optional.

## Building the PC version (Windows)

Double-click `build-pc.bat`. The first time, it installs MSYS2 (a free C
compiler, SDL2 and Python, via winget, into C:\msys64). Then it recompiles
the game and builds `..\build\pc\dqix.exe`. When it finishes, double-click
`..\build\pc\play.bat`. The save file is `..\build\pc\dqix.sav`.

| DS | Keyboard | Controller |
|---|---|---|
| D-pad | arrow keys | D-pad / left stick |
| A / B / X / Y | X / Z / S / A | right / bottom / top / left face button |
| L / R | Q / W | shoulders |
| START / SELECT | Enter / Right Shift | Start / Back |
| touch screen | left mouse button on the bottom screen | |

Every setting is in the **menu bar** at the top of the window (Game,
Graphics, Screen, View, Help); the keys are shortcuts: Tab (hold) = fast
forward, P = pause, F5 = Classic / Enhanced, F4 = 3D resolution in Enhanced,
F6 = DS Lite screen, F7 = DS Lite console, F8 = actual size, F2 = screens side by side,
F3 = window size, F11 or Alt+Enter = fullscreen (move the mouse to the top
edge for the menu bar), F12 = screenshot, F10 = bug report, Esc = leave
fullscreen, or quit.

**Classic and Enhanced.** The game starts in **Classic** mode: the two
screens exactly as a DS draws them (256x192 each, 6-bit colour, the DS's
blending, fading and 3D rasterizer, scaled up with sharp square pixels), with
DS sound (the console's mixer, volume and pitch tables, 10-bit output) and
DS timing (CPU speed, cartridge loading speed). Frames were checked against a
reference DS emulator and match pixel for pixel. **Enhanced** (F5) draws the
3D on the graphics card at 1-4x resolution (F4); the 2D is unchanged. The game
itself runs the same in both modes, so you can switch at any time.

**DS Lite screen (Screen menu, F6).** Makes the screens look like a DS
Lite's instead of exact pixels: the LCD's colours (Pokefan531's measured DS
Lite profile), the red/green/blue stripes and dark lines between pixels
(visible at 3x and larger), the slight motion blur of the LCD, its never
fully black backlight, and four backlight levels. Each part can be turned off.
**View > Show the DS Lite (F7)** draws the console around the screens, at
real proportions (the screens 21.5 mm apart), in six colours; **Gap between
the screens** keeps the real gap without the console. **View > Actual size
(F8)** shows everything as big as a real DS Lite (each screen 61 mm wide),
using the size your monitor reports; if that is wrong, set
`monitor_width_mm` in `settings.ini`. The drawn pixel grid needs about 3
monitor pixels per DS pixel; at actual size that takes a very dense screen
(a high-resolution laptop or tablet). On most desktop monitors the monitor's
own pixels are about as fine as the DS Lite's, so they provide the grid. The game underneath is
still Classic or Enhanced; with the filter off, Classic is pixel-exact.

**Settings.** The game remembers every menu choice in `settings.ini` next to `dqix.exe` (delete it to go
back to the defaults). Enhanced needs OpenGL 3.3 (nearly every PC from the
last 12 years); without it, the game stays in Classic. `set DQIX_SOFTWARE=1`
in `play.bat` forces Classic.

**Versions.** Every update has a version number (in the title bar, Help >
About and `runtime/version.h`).

**Bug reports.** F10 (or a crash) writes `reports/<version>-<date-time>/`
next to the game, so you can see which update a report came from. After F10 a
box opens to describe what went wrong (saved as `description.txt`). Each
report has a screenshot, every input since launch, the save as it was at launch and
now, the clock start, the log, and `perf.txt` (how long each of the last 30
seconds of frames took, split into game code, 2D, 3D and display). The
emulation is deterministic, so replaying a report on the same build
reproduces the session exactly (reports from older builds may play out
differently, because timing changed):

```
DQIX_RTC=<rtc_base from info.txt> DQIX_INPUT_FILE=input.txt ./dqix <rom> <extract> <frame+1> 1 <copy of boot.sav>
```

**Patches.** `patches.txt` overrides individual instructions when the game is
recompiled (the ROM is never changed). It currently removes the anti-piracy
memory leak in the file loader that froze the game after the first battle.

## Status

| | ARM9 (game) | ARM7 (services) |
|---|---|---|
| Functions translated | 14,815 (main, ITCM, 35 overlays) | 962 (found by our own discovery) |
| Instructions translated | 728k, none left untranslated | 38.8k, none left untranslated |
| Decoder vs LLVM disassembler | 723,757 checked, 0 disagreements | 38,704 checked, 0 disagreements |

Behaviour tests (clang-compiled ARM and Thumb functions vs the same C compiled
natively, 24 functions × random inputs): recompiler 192,000 checks and
interpreter 48,000 checks, 0 failures.

## Runtime status

| Piece | State |
|---|---|
| Boot like the DS firmware, both CPUs (binaries, header, chip ID, user settings) | done |
| Firmware flash: Wi-Fi calibration, 3 access-point slots, user settings (with CRCs); writes kept for the session | done |
| ARM9 memory: main RAM, ITCM/DTCM, shared WRAM per WRAMCNT, palette, OAM, VRAM banks | done |
| ARM7 memory: ARM7 WRAM, shared WRAM, VRAM C/D | done |
| CPU modes with banked sp/lr, CP15 | done |
| BIOS (both): Div, Sqrt, CpuSet, CpuFastSet, CRC16, LZ77, RLE, BitUnPack, IntrWait, Halt, WaitByLoop; ARM7 sound tables | done |
| Interrupts, timers (incl. cascade) and DMA per CPU; display timing | done |
| IPC: sync register + both FIFOs with interrupts | done |
| Cartridge: ROM reads (B7/B8) for either CPU, card DMA | done |
| Save chip (64 KB EEPROM on backup SPI), saved to a .sav file | done |
| ARM7 devices: firmware flash (user settings), touch screen, power management, RTC (PC clock + emulated time) | done |
| Game threads on native coroutines, both CPUs (SaveContext / LoadContext / IRQ switch replaced) | done |
| Two CPUs: the ARM7 runs as a coroutine whenever the ARM9 waits or time advances | done |
| Interpreter for code that changes at run time (encrypted anti-piracy, mislabeled code) | done |
| Timing: per-instruction cycle counts (recompiler), cartridge transfer speed, BIOS call cost | done |
| Sound: 16 channels (PCM8/16, IMA-ADPCM, PSG, noise), FIFO timing, capture (reverb), mixer, BIOS volume/pitch tables | done |
| 2D graphics: both engines, all BG modes, sprites, windows, 6-bit blending/brightness, mosaic, display capture | done |
| 3D: geometry engine (FIFO, matrices, lighting, clipping), rasterizer with DS fixed-point rules, textures (all formats), toon/highlight, alpha blending, shadow volumes, anti-aliasing, fog, edge marking | done |
| Input (keyboard, mouse as touch, gamepad) and scripted input for testing | done |

## Running it headless (Linux, for testing)

```
python3 recomp/build.py --extract <extract/usa> --config <config/usa/arm9> --out gen --hle hle.txt --interpret interpret.txt
python3 recomp/arm7.py  --extract <extract/usa> --out gen7 --hle hle7.txt
make -f build.mk GEN=gen GEN7=gen7 DEFS=-DRECOMP_TRACE
./dqix <baserom_dqix_usa.nds> <extract/usa> [frames] [log level 1-3] [save file]
make -f build.mk ... FRONTEND=sdl OUT=dqix-sdl     # with a window
tools/where.sh      # if it hangs: backtrace + both CPUs' state
```

Testing aids (environment variables):

```
DQIX_INPUT="1780:touch=128,160:10;2400:A:20"   scripted input (frame:buttons+touch=x,y:frames)
DQIX_INPUT_FILE=input.txt                      the same from a file (reports use this)
DQIX_WATCH=addr                                log every ARM9 write to that word, with a call trace
DQIX_DUMP=dir,30                               write every 30th frame as .ppm
DQIX_RTC=1760000000                            fixed clock start (reproducible runs)
DQIX_PROF=from,to                              function-entry profile for those frames
DQIX_THREADS=1                                 at exit: every game thread's recent calls
DQIX_TRACEFN=addr,count[,from]                 call trace when a function is entered
DQIX_POLYDUMP=frame,x,y                        3D polygons covering a pixel
DQIX_WAV=out.wav                               record the sound (headless build)
DQIX_SNDLOG=1                                  log sound channel starts
DQIX_CRC=out.txt                               a checksum of every frame (compare runs)
DQIX_DUMP_AT=100,200                           with DQIX_DUMP=dir,1: only those frames
DQIX_MODE=enhanced / DQIX_NO_SETTINGS=1        window build: force a mode / ignore settings.ini
```

## Layout

```
recomp/build.py        ARM9 recompiler: modules, functions, calls, overlays, jump tables
recomp/arm7.py         ARM7 recompiler: discovers functions itself, reuses build.py
recomp/insn.py         ARMv5TE ARM + Thumb instruction decoder that emits C
runtime/cpu.h          CPU state + helpers the generated code uses
runtime/nds.h          runtime state shared by the files below
runtime/mem.c, mem7.c  memory maps (ARM9, ARM7)
runtime/io.c           timing, per-CPU IRQs/timers/DMA, ARM9 registers, divider/sqrt
runtime/io7.c          ARM7 registers: SPI (firmware, touch, power), RTC, sound regs
runtime/card.c         cartridge ROM reads and the save chip
runtime/ipc.c          IPC sync + FIFOs between the CPUs
runtime/cpu_bios.c     CPU modes, interrupt entry, CP15, BIOS (both CPUs), logging
runtime/threads.c      game threads on coroutines (both CPUs) and ARM7 scheduling
runtime/interp.c       ARM/Thumb interpreter for code that isn't recompiled
runtime/coro.h         coroutines: ucontext (Linux) / Fibers (Windows)
runtime/dispatch.c     indirect calls, overlay detection, budgets, call trace
runtime/main.c         ROM loading, firmware boot state, entry
runtime/gpu2d.c        both 2D engines, per scanline
runtime/gpu3d.c        3D geometry engine + software rasterizer
runtime/sound.c        sound channels and mixer
runtime/input.c        DS buttons / touch from the frontend or a script
runtime/frontend_sdl.c window, input, audio, pacing, settings (SDL2)
runtime/menu_win32.c   the menu bar (Windows)
runtime/gl_render.c    OpenGL: Enhanced 3D, composition, the DS Lite screen + console
runtime/frontend_none.c headless frontend (tests)
runtime/report.c       F10 / crash reports (inputs, saves, screenshot, log)
patches.txt            instruction overrides applied when recompiling
build-pc.bat           one-click Windows build (installs MSYS2 first time)
build-windows.sh       the build itself (runs inside MSYS2)
hle.txt / hle7.txt     functions the runtime provides instead (thread switch)
interpret.txt          functions run in the interpreter (encrypted in the ROM)
build.mk               makefile (make -f build.mk)
tools/where.sh         run, then show where the game is
tests/                 decoder check vs LLVM; behaviour tests for recompiler + interpreter
recomp.bat             regenerate the ARM9 C on Windows
```

Generated C is derived from the game: keep it private. The tool contains no game code.

## How it works

* ARM9 function boundaries, modes and overlay targets come from the decomp's
  `symbols.txt` / `relocs.txt`. The ARM7 has no symbol map: `arm7.py` finds
  functions from the entry point, calls, code pointers in literal pools and
  data tables, and prologues in uncovered code.
* Each function becomes `void fn_<module>_<addr>(Cpu* c)`; registers live in
  `c->r[]`. Branches become `goto`, calls C calls, returns `return`.
* Calls into overlays that share an address check which overlay is loaded
  (its code bytes are compared against the overlay file).
* Jump tables (branch tables, address tables, Thumb offset tables, computed
  jumps into unrolled loops) become `switch` statements over local labels.
* Literal-pool loads are folded into constants.
* Indirect calls go through `dispatch_call()`; an address with no recompiled
  function runs in the interpreter.
* `mov lr, pc` / `add lr, pc, #0` followed by a jump is a call. A tail call
  that set a different return address continues there when the callee returns.
* The SDK's thread switch is replaced by native coroutines, one per game
  thread, on both CPUs. The ARM7 runs as another coroutine: whenever the
  ARM9 waits or time moves on it runs until it waits, then hands back.
* Time follows the DS clock. The recompiler adds each straight run of
  instructions' cycle cost; every 512 ARM9 cycles (2,048 ARM7) the runtime
  turns them into bus time, runs the other CPU and delivers interrupts.

## Known limits

* Wireless (local play, Nintendo WFC) is not emulated; the game sees no
  access points.
* Anti-piracy and a few functions the decomp marks as data run in the
  interpreter (correct, but slower).
* 528 `bx r12` long-call stubs go through the dispatcher (later optimisation).
