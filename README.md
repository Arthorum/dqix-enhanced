# DQIX static recompiler (private)

Turns Dragon Quest IX's DS machine code into C that compiles natively.
Milestone 1 of the PC port: **the whole game recompiles, compiles and links.**

## Status

| | |
|---|---|
| Functions translated | 14,815 (main binary, ITCM, all 35 overlays) |
| Instructions translated | 728,349, none left untranslated |
| Calls resolved to a known function | all (654 go through a loaded-overlay check) |
| Decoder vs LLVM disassembler | 723,757 instructions checked, 0 disagreements |
| Behaviour vs native code | 48 test functions × 20,000 random inputs, ARM and Thumb: 0 failures |
| Whole game compiled and linked | yes, with a placeholder runtime |

## Layout

```
recomp/build.py        recompiler: modules, functions, calls, overlays, jump tables
recomp/insn.py         ARMv5TE ARM + Thumb instruction decoder that emits C
runtime/cpu.h          CPU state + helpers the generated code uses
runtime/dispatch.h     table of all recompiled functions (for function pointers)
runtime/stub_runtime.c placeholder runtime, just enough to link
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

## Known limits (next milestones)

* No real runtime yet: memory map, BIOS calls, interrupts, overlay loading,
  graphics, sound and input are all placeholders.
* CPU modes (IRQ/supervisor register banking) are not modelled. Interrupt
  handling will be done in the runtime instead.
* 528 `bx r12` long-call stubs go through the dispatcher. That works, but it's
  slower than a direct call. It's a later optimisation.
* `dispatch_call` in the stub runtime is a linear search: placeholder.
