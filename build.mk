# Build the recompiled game (both CPUs) + runtime (Linux / MSYS2 MinGW on Windows).
#   GEN  = generated ARM9 C (recomp/build.py --hle hle.txt)
#   GEN7 = generated ARM7 C (recomp/arm7.py --hle hle7.txt)
#   FRONTEND = sdl (window, input, sound) or none (headless: frame dumps, scripted input)
#   SDL_CFLAGS / SDL_LIBS default to sdl2-config when FRONTEND=sdl
GEN ?= ../build/recomp
GEN7 ?= ../build/recomp7
CC ?= gcc
CFLAGS ?= -O1 -g -w
OBJ ?= obj
FRONTEND ?= none
OUT ?= dqix

RT = $(wildcard runtime/*.c)
RT := $(filter-out runtime/stub_runtime.c runtime/frontend_%.c,$(RT)) runtime/frontend_$(FRONTEND).c

ifeq ($(FRONTEND),sdl)
SDL_CFLAGS ?= $(shell sdl2-config --cflags)
SDL_LIBS ?= $(shell sdl2-config --libs)
endif

GENOBJ = $(patsubst $(GEN)/%.c,$(OBJ)/gen/%.o,$(wildcard $(GEN)/*.c))
GEN7OBJ = $(patsubst $(GEN7)/%.c,$(OBJ)/gen7/%.o,$(wildcard $(GEN7)/*.c))
RTOBJ = $(patsubst runtime/%.c,$(OBJ)/rt/%.o,$(RT))

$(OUT): $(GENOBJ) $(GEN7OBJ) $(RTOBJ)
	$(CC) -o $@ $^ $(SDL_LIBS) -lm $(LDFLAGS)

$(OBJ)/gen/%.o: $(GEN)/%.c runtime/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEFS) -Iruntime -I$(GEN) -c $< -o $@

$(OBJ)/gen7/%.o: $(GEN7)/%.c runtime/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEFS) -Iruntime -I$(GEN7) -c $< -o $@

$(OBJ)/rt/frontend_sdl.o: runtime/frontend_sdl.c runtime/nds.h runtime/frontend.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -Wall -Wno-unused-parameter $(SDL_CFLAGS) $(DEFS) -Iruntime -c $< -o $@

$(OBJ)/rt/%.o: runtime/%.c runtime/nds.h runtime/cpu.h runtime/coro.h runtime/frontend.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -Wall -Wextra -Wno-unused-parameter $(DEFS) -Iruntime -I$(GEN) -c $< -o $@

clean:
	rm -rf $(OBJ) $(OUT)
