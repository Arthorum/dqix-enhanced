#!/bin/sh
# Build the PC version on Windows. Run by build-pc.bat inside MSYS2 (UCRT64).
# Everything stays in your dqix-decomp folder; the result is build/pc/dqix.exe.
set -e
cd "$(dirname "$0")"
D=..
OUT=$D/build/pc
mkdir -p "$OUT"
echo "== Recompiling the game's code to C (about a minute)"
python recomp/build.py --extract $D/extract/usa --config $D/config/usa/arm9 --out $D/build/recomp --hle hle.txt --interpret interpret.txt --patch patches.txt
python recomp/arm7.py --extract $D/extract/usa --out $D/build/recomp7 --hle hle7.txt
echo "== Compiling (the first time takes a while; later builds only redo what changed)"
make -j"$(nproc)" -f build.mk GEN=$D/build/recomp GEN7=$D/build/recomp7 OBJ=$D/build/obj FRONTEND=sdl OUT="$OUT/dqix.exe" \
     CFLAGS="-O1 -w" DEFS=-DRECOMP_TRACE SDL_CFLAGS="$(sdl2-config --cflags)" SDL_LIBS="-lSDL2" \
     LDFLAGS="-static-libgcc -Wl,--stack,67108864"
cp /ucrt64/bin/SDL2.dll "$OUT/"
cat > "$OUT/play.bat" <<'BAT'
@echo off
rem Dragon Quest IX (PC). Keys: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R,
rem Enter = START, Right Shift = SELECT, mouse = touch screen, Tab = fast forward,
rem F2 = side by side, F3 = window size, F11 = fullscreen, F12 = screenshot, Esc = quit.
cd /d "%~dp0"
dqix.exe ..\..\extract\baserom_dqix_usa.nds ..\..\extract\usa 0 1 dqix.sav
if errorlevel 1 pause
BAT
echo
echo "== Done. Double-click $OUT/play.bat to play."
