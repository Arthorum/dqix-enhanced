# Build the recompiled game (both CPUs) + runtime into ./dqix (Linux / MinGW).
#   GEN  = generated ARM9 C (recomp/build.py --hle hle.txt)
#   GEN7 = generated ARM7 C (recomp/arm7.py --hle hle7.txt)
GEN ?= ../build/recomp
GEN7 ?= ../build/recomp7
CC ?= gcc
CFLAGS ?= -O1 -g -w
RT = $(wildcard runtime/*.c)
RT := $(filter-out runtime/stub_runtime.c,$(RT))
OBJ ?= obj
GENOBJ = $(patsubst $(GEN)/%.c,$(OBJ)/gen/%.o,$(wildcard $(GEN)/*.c))
GEN7OBJ = $(patsubst $(GEN7)/%.c,$(OBJ)/gen7/%.o,$(wildcard $(GEN7)/*.c))
RTOBJ = $(patsubst runtime/%.c,$(OBJ)/rt/%.o,$(RT))

dqix: $(GENOBJ) $(GEN7OBJ) $(RTOBJ)
	$(CC) -o $@ $^ -lm

$(OBJ)/gen/%.o: $(GEN)/%.c runtime/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEFS) -Iruntime -I$(GEN) -c $< -o $@

$(OBJ)/gen7/%.o: $(GEN7)/%.c runtime/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEFS) -Iruntime -I$(GEN7) -c $< -o $@

$(OBJ)/rt/%.o: runtime/%.c runtime/nds.h runtime/cpu.h runtime/coro.h
	@mkdir -p $(dir $@)
	$(CC) -O1 -g -Wall -Wextra -Wno-unused-parameter $(DEFS) -Iruntime -I$(GEN) -c $< -o $@

clean:
	rm -rf $(OBJ) dqix
