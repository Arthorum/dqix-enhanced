# Build the recompiled game + runtime (Linux/MinGW). GEN = generated C from recomp.bat / build.py
GEN ?= ../build/recomp
CC ?= gcc
CFLAGS ?= -O1 -g -w
RT = runtime/mem.c runtime/io.c runtime/cpu_bios.c runtime/ipc_arm7.c runtime/dispatch.c runtime/threads.c runtime/main.c
OBJ = obj
GENSRC = $(wildcard $(GEN)/*.c)
GENOBJ = $(patsubst $(GEN)/%.c,$(OBJ)/gen/%.o,$(GENSRC))
RTOBJ = $(patsubst runtime/%.c,$(OBJ)/rt/%.o,$(RT))

dqix: $(GENOBJ) $(RTOBJ)
	$(CC) -o $@ $^ -lm

$(OBJ)/gen/%.o: $(GEN)/%.c runtime/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEFS) -Iruntime -I$(GEN) -c $< -o $@

$(OBJ)/rt/%.o: runtime/%.c runtime/nds.h runtime/cpu.h
	@mkdir -p $(dir $@)
	$(CC) -O1 -g -Wall -Wextra -Wno-unused-parameter $(DEFS) -Iruntime -I$(GEN) -c $< -o $@

clean:
	rm -rf $(OBJ) dqix
