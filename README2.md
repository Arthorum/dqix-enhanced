# DQIX static recompiler (private)

Turns Dragon Quest IX's DS machine code into C that compiles natively, plus
the runtime that stands in for the DS hardware.

* Milestone 1 (done): the whole game recompiles, compiles and links.
* Milestone 2 (in progress): the game boots on PC. Startup, OS init, interrupts,
  timers and game threads run; it reaches its first file load, which needs the ARM7.

## Status

| | |
|---|---|
| Functions translated | 14,815 (main binary, ITCM, all 35 overlays) |
| Instructions translated | 728,349, none left untranslated |
| Calls resolved to a known function | all (654 go through a loaded-overlay check) |
| Decoder vs LLVM disassembler | 723,757 instructions checked, 0 disagreements |
| Behaviour vs native code | 48 test functions × 20,000 random inputs, ARM and Thumb: 0 failures |
| Whole game compiled and linked | yes, with a placeholder runtime |

## Runtime status (milestone 2)

| Piece | State |
|---|---|
| Boot like the DS firmware (ARM9 binary from ROM, header, chip ID, user settings) | done |
| Memory map: main RAM, ITCM/DTCM, shared WRAM, palette, OAM, VRAM bank mapping | done |
| CPU modes with banked sp/lr, CP15 (TCM setup, wait-for-interrupt) | done |
| BIOS calls: Div, Sqrt, CpuSet, CpuFastSet, CRC16, LZ77, RLE, BitUnPack, IntrWait, Halt | done |
| Interrupts delivered at wait points and when enabled; IRQ stack frame like the BIOS | done |
| Display timing (VCOUNT/DISPSTAT), timers incl. cascade, DMA (immediate/VBlank/HBlank/card), hardware divider + sqrt | done |
| Cartridge reads (command B7 / B8) | done, not exercised yet |
| Game threads: OS_SaveContext / OS_LoadContext / IRQ thread switch on native coroutines | done |
| ARM7: PXI boot handshake + "services ready" bits | stand-in only |
| ARM7 services (card/saves, sound, touch, RTC, power) | next: recompile the ARM7 program too |
| 2D / 3D graphics, sound output, input | later |

Run it (Linux; the Windows build comes with Claude Code on the PC):

```
python3 recomp/build.py --extract <extract/usa> --config <config/usa/arm9> --out gen --hle hle.txt
make -f build.mk GEN=gen DEFS=-DRECOMP_TRACE
./dqix <baserom_dqix_usa.nds> <extract/usa> [frames] [log level 1-3]
tools/where.sh      # if it hangs: backtrace + CPU state
```

`hle.txt` lists game functions the runtime implements natively (the thread switch).

## Layout

```
recomp/build.py        recompiler: modules, functions, calls, overlays, jump tables
recomp/insn.py         ARMv5TE ARM + Thumb instruction decoder that emits C
runtime/cpu.h          CPU state + helpers the generated code uses
runtime/dispatch.h     table of all recompiled functions (for function pointers)
runtime/nds.h          runtime state shared between the files below
runtime/mem.c          ARM9 memory map, VRAM bank mapping
runtime/io.c           I/O registers: timing, IRQs, timers, DMA, cartridge, divider/sqrt
runtime/cpu_bios.c     CPU modes, interrupt entry, CP15, BIOS calls, logging
runtime/threads.c      game threads on native coroutines (replaces SDK context switch)
runtime/coro.h         coroutines: ucontext (Linux) / Fibers (Windows)
runtime/ipc_arm7.c     ARM9<->ARM7 IPC + stand-in ARM7
runtime/dispatch.c     fast lookup for indirect calls, overlay detection, call trace
runtime/main.c         ROM loading, firmware boot state, entry
runtime/stub_runtime.c placeholder runtime, just enough to link (milestone 1)
hle.txt                functions provided by the runtime instead of recompiled
build.mk               makefile: builds generated C + runtime into ./dqix (make -f build.mk)
tools/where.sh         run, then show where the game is (backtrace + registers)
tests/verify_decode.py checks every decoded instruction against llvm-objdump
tests/semantics/       clang-compiled ARM/Thumb tests, recompiled and run vs native
recomp.bat             regenerate the C on Windows
```

## Running it

Needs the dqix-decomp checkout next to it with a successful `ninja check rom`
(that extracts the game's code into `extract\usa`).

```
recomp.bat
```

writes the generated C to `..\build\recomp` (about 57 MB, 62 files). That output
is derived from the game: keep it private, never commit or share it. The tool
itself contains no game code.

The test scripts currently run on Linux (they need clang, ld.lld, llvm-objdump, gcc).

## How it works

* Function boundaries, ARM/Thumb mode and overlay targets come from the
  decomp's `config/usa/arm9/*/symbols.txt` and `relocs.txt`.
* Each function becomes `void fn_<module>_<addr>(Cpu* c)`. Registers live in
  `c->r[]`, so recompiled code, hand-decompiled code and the runtime can call
  each other with the normal ARM calling convention.
* Branches inside a function become `goto`; calls become C calls; returns
  (`bx lr`, `pop {pc}`, `ldm sp!, {..., pc}`) become `return`.
* Calls into overlays that share an address check which overlay is loaded.
* Jump tables (`addls pc, pc, rX, lsl #2`, address tables, Thumb offset
  tables) become `switch` statements over local labels.
* Literal-pool loads are folded into constants at translation time.
* Anything indirect falls back to `dispatch_call()`, which looks up the target
  in the function table.
* A tail call that set a different return address first (the SDK interrupt
  dispatcher does this) continues at that address when the callee returns.

## Known limits

* The ARM7 is a stand-in: the game's first card request goes unanswered.
* No graphics, sound or input yet.
* 528 `bx r12` long-call stubs go through the dispatcher. That works, but it's
  slower than a direct call. It's a later optimisation.
* Time only advances at waits and status-register polls; tight loops on plain
  memory would spin. None seen so far.
