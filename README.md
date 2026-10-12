# DQIX static recompiler (private)

Turns Dragon Quest IX's DS machine code (both CPUs) into C that compiles
natively, plus the runtime that stands in for the DS hardware.

* Milestone 1 (done): the whole game recompiles, compiles and links.
* Milestone 2 (done): the game boots on PC with both CPUs. It runs its startup,
  OS, threads, the ARM7's services and anti-piracy, and loads files and
  overlays from the cartridge. Its main loop runs every frame (no picture yet).
* Milestone 3 (next): graphics, so we can see it. 2D engines first (logos,
  title, menus), then 3D.

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
| ARM9 memory: main RAM, ITCM/DTCM, shared WRAM per WRAMCNT, palette, OAM, VRAM banks | done |
| ARM7 memory: ARM7 WRAM, shared WRAM, VRAM C/D | done |
| CPU modes with banked sp/lr, CP15 | done |
| BIOS (both): Div, Sqrt, CpuSet, CpuFastSet, CRC16, LZ77, RLE, BitUnPack, IntrWait, Halt, WaitByLoop; ARM7 sound tables | done |
| Interrupts, timers (incl. cascade) and DMA per CPU; display timing | done |
| IPC: sync register + both FIFOs with interrupts | done |
| Cartridge: ROM reads (B7/B8) for either CPU, card DMA | done |
| Save chip on backup SPI (flash command set), saved to a .sav file | done, not exercised yet |
| ARM7 devices: firmware flash (user settings), touch screen (no touch), power management, RTC with the PC clock | done |
| Game threads on native coroutines, both CPUs (SaveContext / LoadContext / IRQ switch replaced) | done |
| Two CPUs: the ARM7 runs as a coroutine whenever the ARM9 waits or time advances | done |
| Interpreter for code that changes at run time (encrypted anti-piracy, mislabeled code) | done |
| Forward progress: call + loop budgets so busy-waits see time pass and interrupts arrive | done |
| Sound channels | registers only; one-shot sounds end at once (no mixer yet) |
| Graphics, sound output, input | next |

## Running it (Linux; Windows build comes with Claude Code on the PC)

```
python3 recomp/build.py --extract <extract/usa> --config <config/usa/arm9> --out gen --hle hle.txt --interpret interpret.txt
python3 recomp/arm7.py  --extract <extract/usa> --out gen7 --hle hle7.txt
make -f build.mk GEN=gen GEN7=gen7 DEFS=-DRECOMP_TRACE
./dqix <baserom_dqix_usa.nds> <extract/usa> [frames] [log level 1-3] [save file]
tools/where.sh      # if it hangs: backtrace + both CPUs' state
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
* Time is counted in scanlines. It advances at waits, at status-register
  polls, every 1,024 loop iterations and every ~1M calls.

## Known limits

* No graphics, sound or input yet.
* Anti-piracy and a few functions the decomp marks as data run in the
  interpreter (correct, but slower).
* 528 `bx r12` long-call stubs go through the dispatcher (later optimisation).
